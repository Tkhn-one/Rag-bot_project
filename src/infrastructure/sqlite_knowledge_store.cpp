#include "rag/infrastructure/sqlite_knowledge_store.hpp"
#include "rag/infrastructure/utf8.hpp"

#include <sqlite3.h>

namespace rag {
namespace {

constexpr int applicationId = 0x52414742;  // RAGB
constexpr int schemaVersion = 1;
constexpr auto createSchema = R"sql(
    CREATE TABLE documents (
        id INTEGER PRIMARY KEY,
        source TEXT NOT NULL,
        content TEXT NOT NULL COLLATE BINARY UNIQUE,
        policy INTEGER NOT NULL CHECK (policy IN (0, 1))
    ) STRICT;
    CREATE TABLE chunks (
        document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
        line INTEGER NOT NULL CHECK (line > 0),
        text TEXT NOT NULL,
        PRIMARY KEY (document_id, line)
    ) STRICT, WITHOUT ROWID;
    PRAGMA application_id = 0x52414742;
    PRAGMA user_version = 1;
)sql";

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    return Statement(statement, sqlite3_finalize);
}

bool execute(sqlite3* database, const char* sql) {
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

class WriteTransaction {
public:
    explicit WriteTransaction(sqlite3* database)
        : database_(database), active_(execute(database, "BEGIN IMMEDIATE")) {}

    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;

    ~WriteTransaction() {
        if (active_) {
            execute(database_, "ROLLBACK");
        }
    }

    bool started() const { return active_; }

    bool commit() {
        if (!execute(database_, "COMMIT")) {
            return false;
        }
        active_ = false;
        return true;
    }

private:
    sqlite3* database_;
    bool active_;
};

AppError databaseError(sqlite3* database, AppError fallback) {
    const auto code = sqlite3_errcode(database);
    if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB) {
        return AppError::StoreCorrupt;
    }
    return fallback;
}

std::optional<std::string> readText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    if (!value) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

bool bindText(sqlite3_stmt* statement, int index, const std::string& text) {
    return sqlite3_bind_text64(statement, index, text.data(), text.size(), SQLITE_TRANSIENT,
                               SQLITE_UTF8) == SQLITE_OK;
}

struct Schema {
    int application;
    int version;
    bool hasObjects;

    bool supported() const { return application == applicationId && version == schemaVersion; }
    bool uninitialized() const { return application == 0 && version == 0 && !hasObjects; }
};

std::optional<Schema> readSchema(sqlite3* database) {
    const auto query = prepare(database, R"sql(
        SELECT application_id, user_version, EXISTS (SELECT 1 FROM sqlite_schema)
        FROM pragma_application_id(), pragma_user_version()
    )sql");
    if (!query || sqlite3_step(query.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    return Schema{sqlite3_column_int(query.get(), 0), sqlite3_column_int(query.get(), 1),
                  sqlite3_column_int(query.get(), 2) != 0};
}

std::optional<AppError> initializeSchema(sqlite3* database) {
    WriteTransaction transaction(database);
    if (!transaction.started()) {
        return databaseError(database, AppError::StoreWriteFailed);
    }

    // Another process may have initialized the file before we acquired the write lock.
    const auto schema = readSchema(database);
    if (!schema) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    if (!schema->supported()) {
        if (!schema->uninitialized()) {
            return AppError::StoreSchemaUnsupported;
        }
        if (!execute(database, createSchema)) {
            return databaseError(database, AppError::StoreWriteFailed);
        }
    }
    if (!transaction.commit()) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    return std::nullopt;
}

Result<std::size_t> duplicateChunkCount(sqlite3* database, const std::string& content) {
    const auto query = prepare(database, R"sql(
        SELECT (SELECT count(*) FROM chunks WHERE document_id = documents.id)
        FROM documents WHERE content = ?1
    )sql");
    if (!query || !bindText(query.get(), 1, content) || sqlite3_step(query.get()) != SQLITE_ROW) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    const auto count = sqlite3_column_int64(query.get(), 0);
    if (count <= 0 || !std::in_range<std::size_t>(count)) {
        return AppError::StoreCorrupt;
    }
    return static_cast<std::size_t>(count);
}

Result<std::size_t> writeDocument(sqlite3* database, const std::string& source,
                                  const std::string& content, std::span<const DocumentLine> lines,
                                  DocumentPolicy policy) {
    const auto document = prepare(database, R"sql(
        INSERT INTO documents (source, content, policy) VALUES (?1, ?2, ?3)
        ON CONFLICT (content) DO NOTHING
    )sql");
    if (!document || !bindText(document.get(), 1, source) || !bindText(document.get(), 2, content) ||
        sqlite3_bind_int(document.get(), 3, policy == DocumentPolicy::AllowRemote ? 1 : 0) != SQLITE_OK ||
        sqlite3_step(document.get()) != SQLITE_DONE) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    if (sqlite3_changes(database) == 0) {
        return duplicateChunkCount(database, content);
    }

    const auto id = sqlite3_last_insert_rowid(database);
    const auto chunk = prepare(database,
        "INSERT INTO chunks (document_id, line, text) VALUES (?1, ?2, ?3)");
    if (!chunk || sqlite3_bind_int64(chunk.get(), 1, id) != SQLITE_OK) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    for (const auto& line : lines) {
        if (!std::in_range<sqlite3_int64>(line.number) ||
            sqlite3_bind_int64(chunk.get(), 2, static_cast<sqlite3_int64>(line.number)) != SQLITE_OK ||
            !bindText(chunk.get(), 3, line.text) || sqlite3_step(chunk.get()) != SQLITE_DONE ||
            sqlite3_reset(chunk.get()) != SQLITE_OK) {
            return databaseError(database, AppError::StoreWriteFailed);
        }
    }
    return lines.size();
}

Result<std::vector<Chunk>> readChunks(sqlite3* database, sqlite3_stmt* query, sqlite3_int64 id) {
    if (sqlite3_reset(query) != SQLITE_OK || sqlite3_bind_int64(query, 1, id) != SQLITE_OK) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    std::vector<Chunk> chunks;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(query)) == SQLITE_ROW) {
        const auto line = sqlite3_column_int64(query, 0);
        auto text = readText(query, 1);
        if (!text || line <= 0 || !std::in_range<std::size_t>(line)) {
            return AppError::StoreCorrupt;
        }
        chunks.emplace_back(static_cast<DocumentId>(id), static_cast<std::size_t>(line),
                            std::move(*text));
    }
    if (status != SQLITE_DONE) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    if (chunks.empty()) {
        return AppError::StoreCorrupt;
    }
    return chunks;
}

}  // namespace

void SqliteKnowledgeStore::CloseDatabase::operator()(sqlite3* database) const noexcept {
    sqlite3_close(database);
}

Result<std::unique_ptr<SqliteKnowledgeStore>> SqliteKnowledgeStore::open(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return AppError::StoreUnavailable;
    }
    std::error_code pathError;
    const auto absolutePath = std::filesystem::absolute(path, pathError);
    if (pathError) {
        return AppError::StoreUnavailable;
    }
    const bool existed = std::filesystem::exists(absolutePath, pathError);
    const auto filename = encodeUtf8(absolutePath.native());
    if (pathError || !filename) {
        return AppError::StoreUnavailable;
    }

    sqlite3* raw = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | (existed ? 0 : SQLITE_OPEN_CREATE);
    const auto opened = sqlite3_open_v2(filename->c_str(), &raw, flags, nullptr);
    Database database(raw);
    if (opened != SQLITE_OK) {
        return database ? databaseError(raw, AppError::StoreUnavailable) : AppError::StoreUnavailable;
    }
    if (sqlite3_busy_timeout(raw, 3000) != SQLITE_OK || !execute(raw, "PRAGMA foreign_keys = ON")) {
        return databaseError(raw, AppError::StoreUnavailable);
    }

    const auto schema = readSchema(raw);
    if (!schema) {
        return databaseError(raw, AppError::StoreReadFailed);
    }
    if (!schema->supported()) {
        if (existed || !schema->uninitialized()) {
            if (schema->uninitialized() && std::filesystem::file_size(absolutePath, pathError) == 0) {
                return AppError::StoreCorrupt;
            }
            return AppError::StoreSchemaUnsupported;
        }
        if (const auto error = initializeSchema(raw)) {
            return *error;
        }
    }

    const auto check = prepare(raw, "PRAGMA quick_check(1)");
    if (!check || sqlite3_step(check.get()) != SQLITE_ROW) {
        return databaseError(raw, AppError::StoreReadFailed);
    }
    if (readText(check.get(), 0) != "ok") {
        return AppError::StoreCorrupt;
    }
    return std::unique_ptr<SqliteKnowledgeStore>(new SqliteKnowledgeStore(std::move(database)));
}

Result<std::size_t> SqliteKnowledgeStore::save(std::string source, std::string content,
                                              std::vector<DocumentLine> lines, DocumentPolicy policy) {
    if (lines.empty()) {
        return AppError::StoreWriteFailed;
    }
    WriteTransaction transaction(database_.get());
    if (!transaction.started()) {
        return databaseError(database_.get(), AppError::StoreWriteFailed);
    }

    const auto result = writeDocument(database_.get(), source, content, lines, policy);
    if (std::holds_alternative<AppError>(result)) {
        return result;
    }
    if (!transaction.commit()) {
        return databaseError(database_.get(), AppError::StoreWriteFailed);
    }
    return result;
}

Result<std::vector<IndexedDocument>> SqliteKnowledgeStore::documents() const {
    auto* database = database_.get();
    const auto documents = prepare(database, "SELECT id, source, content, policy FROM documents ORDER BY id");
    const auto chunks = prepare(database, "SELECT line, text FROM chunks WHERE document_id = ?1 ORDER BY line");
    if (!documents || !chunks) {
        return databaseError(database, AppError::StoreReadFailed);
    }

    std::vector<IndexedDocument> result;
    int status = SQLITE_OK;
    // The active document statement keeps the read snapshot open while reading its chunks.
    while ((status = sqlite3_step(documents.get())) == SQLITE_ROW) {
        const auto id = sqlite3_column_int64(documents.get(), 0);
        auto source = readText(documents.get(), 1);
        auto content = readText(documents.get(), 2);
        const auto policy = sqlite3_column_int(documents.get(), 3);
        if (!source || !content || id <= 0 || !std::in_range<DocumentId>(id) ||
            (policy != 0 && policy != 1)) {
            return AppError::StoreCorrupt;
        }
        auto storedChunks = readChunks(database, chunks.get(), id);
        if (const auto* error = std::get_if<AppError>(&storedChunks)) {
            return *error;
        }
        result.emplace_back(static_cast<DocumentId>(id), std::move(*source), std::move(*content),
                            std::get<std::vector<Chunk>>(std::move(storedChunks)),
                            policy == 1 ? DocumentPolicy::AllowRemote : DocumentPolicy::LocalOnly);
    }
    if (status != SQLITE_DONE) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    return result;
}

}  // namespace rag
