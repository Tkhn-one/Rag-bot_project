#include "rag/infrastructure/sqlite_knowledge_store.hpp"
#include "sqlite_helpers.hpp"
#include "sqlite_search.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace rag {
namespace {
using namespace sql;
using Key = std::pair<DocumentId, std::size_t>;
struct Ranked { Key key; double score; };

bool higher(const Ranked& a, const Ranked& b) {
    return a.score == b.score ? a.key < b.key : a.score > b.score;
}

std::string settingsId(SearchSettings s) {
    return "unicode61-bm25-cos-rrf-v1/" + std::to_string(s.candidates) + "/" + std::to_string(s.rrfConstant) +
           "/" + std::to_string(s.contextLimit) + "/" + std::to_string(s.perDocument) + "/blocks400-60-v1";
}

Result<std::vector<float>> normalized(const IEmbeddingProvider& provider, std::string_view text, EmbeddingInput input) {
    auto result = provider.embed(text, input);
    if (auto* values = std::get_if<std::vector<float>>(&result)) {
        if (values->size() != provider.dimension()) {
            return AppError::EmbeddingFailed;
        }
        double norm = 0;
        for (float value : *values) {
            norm += static_cast<double>(value) * value;
        }
        if (!(norm > 0) || !std::isfinite(norm)) {
            return AppError::EmbeddingFailed;
        }
        norm = std::sqrt(norm);
        for (auto& value : *values) {
            value = static_cast<float>(value / norm);
        }
    }
    return result;
}

// Let the same unicode61 tokenizer parse the question. MATCH receives only quoted tokens joined by OR.
int appendToken(void* context, int, const char* token, int size, int, int) {
    auto& expression = *static_cast<std::string*>(context);
    if (!expression.empty()) expression += " OR ";
    expression += '"';
    for (char c : std::string_view(token, size)) {
        if (c == '"') expression += '"';
        expression += c;
    }
    expression += '"';
    return SQLITE_OK;
}

Result<std::vector<Key>> lexicalRanks(sqlite3* db, const Question& question, int limit) {
    fts5_api* api = nullptr;
    auto getApi = prepare(db, "SELECT fts5(?1)");
    if (!getApi || sqlite3_bind_pointer(getApi.get(), 1, &api, "fts5_api_ptr", nullptr) != SQLITE_OK ||
        sqlite3_step(getApi.get()) != SQLITE_ROW || !api) return AppError::StoreReadFailed;
    void* context = nullptr;
    fts5_tokenizer tokenizer{};
    Fts5Tokenizer* instance = nullptr;
    if (api->xFindTokenizer(api, "unicode61", &context, &tokenizer) != SQLITE_OK ||
        tokenizer.xCreate(context, nullptr, 0, &instance) != SQLITE_OK) return AppError::StoreReadFailed;
    const std::unique_ptr<Fts5Tokenizer, decltype(tokenizer.xDelete)> owner(instance, tokenizer.xDelete);
    std::string expression;
    if (!std::in_range<int>(question.text().size()) ||
        tokenizer.xTokenize(instance, &expression, FTS5_TOKENIZE_QUERY, question.text().data(),
                            static_cast<int>(question.text().size()), appendToken) != SQLITE_OK) {
        return AppError::StoreReadFailed;
    }
    std::vector<Key> keys;
    if (expression.empty()) return keys;
    auto stmt = prepare(db, "SELECT document_id,line FROM chunks_fts WHERE chunks_fts MATCH ?1 "
                            "ORDER BY bm25(chunks_fts),document_id,line LIMIT ?2");
    if (!stmt || !bindText(stmt.get(), 1, expression) || sqlite3_bind_int(stmt.get(), 2, limit) != SQLITE_OK)
        return AppError::StoreReadFailed;
    int status;
    while ((status = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        keys.emplace_back(sqlite3_column_int64(stmt.get(), 0), sqlite3_column_int64(stmt.get(), 1));
    }
    if (status != SQLITE_DONE) return AppError::StoreReadFailed;
    return keys;
}

Result<std::vector<Key>> vectorRanks(sqlite3* db, const IEmbeddingProvider& provider, const Question& question,
                                     std::size_t limit, std::size_t expectedCount) {
    auto result = normalized(provider, question.text(), EmbeddingInput::Query);
    if (const auto* error = std::get_if<AppError>(&result)) return *error;
    const auto& query = std::get<std::vector<float>>(result);
    auto stmt = prepare(db, "SELECT document_id,line,model,dimension,vector FROM embeddings");
    if (!stmt) return AppError::StoreReadFailed;
    const auto model = provider.profileId();
    std::vector<Ranked> best;
    std::size_t count = 0;
    int status;
    while ((status = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        ++count;
        const auto* bytes = static_cast<const unsigned char*>(sqlite3_column_blob(stmt.get(), 4));
        if (!bytes || readText(stmt.get(), 2) != model ||
            sqlite3_column_int64(stmt.get(), 3) != static_cast<sqlite3_int64>(query.size()) ||
            sqlite3_column_bytes(stmt.get(), 4) != static_cast<sqlite3_int64>(query.size() * sizeof(float))) {
            return AppError::StoreCorrupt;
        }
        double cosine = 0;
        for (std::size_t i = 0; i < query.size(); ++i) {
            float stored;
            std::memcpy(&stored, bytes + i * sizeof(float), sizeof(float));
            cosine += static_cast<double>(stored) * query[i];
        }
        if (!std::isfinite(cosine)) return AppError::StoreCorrupt;
        Ranked row{{sqlite3_column_int64(stmt.get(), 0), sqlite3_column_int64(stmt.get(), 1)}, cosine};
        const auto pos = std::lower_bound(best.begin(), best.end(), row, higher);
        if (best.size() < limit || pos != best.end()) {
            best.insert(pos, row);
            if (best.size() > limit) best.pop_back();
        }
    }
    if (status != SQLITE_DONE) return AppError::StoreReadFailed;
    if (count != expectedCount) return AppError::StoreCorrupt;
    std::vector<Key> keys;
    for (const auto& row : best) keys.push_back(row.key);
    return keys;
}

std::vector<Key> fuse(const std::vector<Key>& lexical, const std::vector<Key>& semantic, SearchSettings settings) {
    std::map<Key, double> scores;
    for (auto* list : {&lexical, &semantic}) {
        for (std::size_t i = 0; i < list->size(); ++i) {
            scores[(*list)[i]] += 1.0 / (settings.rrfConstant + static_cast<double>(i) + 1);
        }
    }
    std::vector<Ranked> ranking;
    for (const auto& [key, score] : scores) ranking.push_back({key, score});
    std::ranges::sort(ranking, higher);
    std::vector<Key> selected, deferred;
    std::map<DocumentId, int> counts;
    for (const auto& row : ranking) {
        if (counts[row.key.first]++ < settings.perDocument) selected.push_back(row.key);
        else deferred.push_back(row.key);
    }
    selected.insert(selected.end(), deferred.begin(), deferred.end());
    if (selected.size() > static_cast<std::size_t>(settings.contextLimit)) selected.resize(settings.contextLimit);
    return selected;
}
}  // namespace

Result<bool> searchProfileMatches(sqlite3* db, const IEmbeddingProvider* provider, SearchSettings settings) {
    auto row = sql::prepare(db, "SELECT model,dimension,settings FROM search_profile WHERE id=1");
    if (!row || sqlite3_step(row.get()) != SQLITE_ROW) return AppError::StoreReadFailed;
    return sql::readText(row.get(), 0) == (provider ? provider->profileId() : "") &&
           sqlite3_column_int64(row.get(), 1) == static_cast<sqlite3_int64>(provider ? provider->dimension() : 0) &&
           sql::readText(row.get(), 2) == settingsId(settings);
}

Result<bool> indexVector(sqlite3* db, const IEmbeddingProvider& provider, std::int64_t id,
                         std::size_t line, std::string_view text) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559 && std::endian::native == std::endian::little);
    auto result = normalized(provider, text, EmbeddingInput::Document);
    if (const auto* error = std::get_if<AppError>(&result)) return *error;
    auto& vector = std::get<std::vector<float>>(result);
    auto stmt = sql::prepare(db, "INSERT INTO embeddings(document_id,line,model,dimension,vector) VALUES(?1,?2,?3,?4,?5)");
    if (!stmt || sqlite3_bind_int64(stmt.get(), 1, id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(line)) != SQLITE_OK ||
        !sql::bindText(stmt.get(), 3, provider.profileId()) ||
        sqlite3_bind_int64(stmt.get(), 4, static_cast<sqlite3_int64>(vector.size())) != SQLITE_OK ||
        sqlite3_bind_blob64(stmt.get(), 5, vector.data(), vector.size()*sizeof(float), SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(stmt.get()) != SQLITE_DONE) return AppError::StoreWriteFailed;
    return true;
}

Result<bool> SqliteKnowledgeStore::configureSearch(const IEmbeddingProvider* provider, SearchSettings settings, bool rebuild) {
    if (settings.candidates <= 0 || settings.contextLimit <= 0 || settings.perDocument <= 0 || settings.rrfConstant <= 0 ||
        (provider && (provider->profileId().empty() || provider->dimension() == 0))) return AppError::SearchProfileMismatch;
    auto* db = database_.get();
    sql::WriteTransaction txn(db);
    if (!txn.started()) return AppError::StoreWriteFailed;
    auto match = searchProfileMatches(db, provider, settings);
    if (const auto* error = std::get_if<AppError>(&match)) return *error;
    const bool changed = !std::get<bool>(match) || rebuild;
    if (changed) {
        auto hasChunks = sql::prepare(db, "SELECT EXISTS(SELECT 1 FROM chunks)");
        if (!hasChunks || sqlite3_step(hasChunks.get()) != SQLITE_ROW) return AppError::StoreReadFailed;
        if (!rebuild && sqlite3_column_int(hasChunks.get(), 0)) return AppError::SearchProfileMismatch;
        const auto rebuilt = rebuildSearchChunks(db, provider);
        if (const auto* error = std::get_if<AppError>(&rebuilt)) return *error;
        auto update = sql::prepare(db, "UPDATE search_profile SET model=?1,dimension=?2,settings=?3 WHERE id=1");
        if (!update || !sql::bindText(update.get(), 1, provider ? provider->profileId() : "") ||
            sqlite3_bind_int64(update.get(), 2, provider ? provider->dimension() : 0) != SQLITE_OK ||
            !sql::bindText(update.get(), 3, settingsId(settings)) || sqlite3_step(update.get()) != SQLITE_DONE ||
            !sql::execute(db, "UPDATE library SET revision=revision+1 WHERE id=1")) return AppError::StoreWriteFailed;
    }
    if (!txn.commit()) return AppError::StoreWriteFailed;
    embeddings_ = provider;
    settings_ = settings;
    return changed;
}

Result<std::vector<Chunk>> SqliteKnowledgeStore::retrieve(const Question& question, const LibrarySnapshot& library) const {
    auto* db = database_.get();
    // The active cursor pins the read transaction across both rankings. Never mix a newer index with an admitted snapshot.
    auto version = sql::prepare(db, "SELECT library_id,revision FROM library WHERE id=1");
    if (!version || sqlite3_step(version.get()) != SQLITE_ROW) return AppError::StoreReadFailed;
    if (sql::readText(version.get(), 0) != library.revision.libraryId ||
        sqlite3_column_int64(version.get(), 1) != static_cast<sqlite3_int64>(library.revision.number))
        return AppError::SearchProfileMismatch;
    const auto match = searchProfileMatches(db, embeddings_, settings_);
    if (const auto* error = std::get_if<AppError>(&match)) return *error;
    if (!std::get<bool>(match)) return AppError::SearchProfileMismatch;
    const auto lexical = lexicalRanks(db, question, settings_.candidates);
    if (const auto* error = std::get_if<AppError>(&lexical)) return *error;
    Result<std::vector<Key>> semantic = std::vector<Key>{};
    if (embeddings_) {
        std::size_t expectedCount = 0;
        for (const auto& document : library.documents) expectedCount += document.chunks().size();
        semantic = vectorRanks(db, *embeddings_, question, settings_.candidates, expectedCount);
    }
    if (const auto* error = std::get_if<AppError>(&semantic)) return *error;
    std::vector<Chunk> chunks;
    for (const auto& [id, line] : fuse(std::get<std::vector<Key>>(lexical), std::get<std::vector<Key>>(semantic), settings_)) {
        const auto doc = std::ranges::lower_bound(library.documents, id, {}, &IndexedDocument::id);
        if (doc == library.documents.end() || doc->id() != id) return AppError::StoreCorrupt;
        const auto chunk = std::ranges::lower_bound(doc->chunks(), line, {}, &Chunk::chunkId);
        if (chunk == doc->chunks().end() || chunk->chunkId() != line) return AppError::StoreCorrupt;
        chunks.push_back(*chunk);
    }
    return chunks;
}
}  // namespace rag
