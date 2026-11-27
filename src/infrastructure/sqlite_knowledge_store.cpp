#include "rag/infrastructure/sqlite_knowledge_store.hpp"
#include "rag/infrastructure/utf8.hpp"
#include "cache_key.hpp"
#include "managed_source.hpp"
#include "rag/infrastructure/local_path.hpp"
#include "rag/application/services.hpp"
#include "rag/infrastructure/block_slices.hpp"
#include "sqlite_search.hpp"
#include "sqlite_helpers.hpp"

#include <sqlite3.h>
#include <algorithm>

namespace rag {
namespace {

constexpr int applicationId = 0x52414742;  // RAGB
constexpr int schemaVersion = 11;
constexpr auto createV2Schema = R"sql(
    CREATE TABLE documents (
        id INTEGER PRIMARY KEY,
        source TEXT NOT NULL,
        content TEXT NOT NULL COLLATE BINARY,
        policy INTEGER NOT NULL CHECK (policy IN (0, 1)),
        page_layout TEXT NOT NULL,
        UNIQUE (content, page_layout)
    ) STRICT;
    CREATE TABLE chunks (
        document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
        line INTEGER NOT NULL CHECK (line > 0),
        text TEXT NOT NULL,
        page INTEGER CHECK (page > 0),
        PRIMARY KEY (document_id, line)
    ) STRICT, WITHOUT ROWID;
    PRAGMA application_id = 0x52414742;
    PRAGMA user_version = 2;
)sql";
constexpr auto addDocxPositions = R"sql(
    ALTER TABLE documents RENAME COLUMN page_layout TO position_layout;
    ALTER TABLE chunks ADD COLUMN docx_part TEXT;
    ALTER TABLE chunks ADD COLUMN docx_paragraph INTEGER CHECK (docx_paragraph > 0);
    ALTER TABLE chunks ADD COLUMN docx_table INTEGER CHECK (docx_table > 0);
    ALTER TABLE chunks ADD COLUMN docx_row INTEGER CHECK (docx_row > 0);
    ALTER TABLE chunks ADD COLUMN docx_cell INTEGER CHECK (docx_cell > 0) CHECK (
        (docx_part IS NULL AND docx_paragraph IS NULL AND docx_table IS NULL AND
         docx_row IS NULL AND docx_cell IS NULL)
        OR
        (docx_part IS NOT NULL AND length(docx_part) > 0 AND page IS NULL AND (
            (docx_paragraph IS NOT NULL AND docx_table IS NULL AND docx_row IS NULL AND docx_cell IS NULL)
            OR
            (docx_paragraph IS NULL AND docx_table IS NOT NULL AND docx_row IS NOT NULL AND docx_cell IS NOT NULL)
        ))
    );
    PRAGMA user_version = 3;
)sql";
constexpr auto addLibraryMetadata = R"sql(
    CREATE TABLE library (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        library_id TEXT NOT NULL CHECK (length(library_id) = 32),
        revision INTEGER NOT NULL CHECK (revision >= 0),
        policy INTEGER NOT NULL CHECK (policy IN (0, 1))
    ) STRICT;
    INSERT INTO library (id, library_id, revision, policy)
        VALUES (1, lower(hex(randomblob(16))), 0, 0);
    PRAGMA user_version = 4;
)sql";

constexpr auto addCacheAndHistory = R"sql(
    CREATE TABLE answer_cache (
        key BLOB PRIMARY KEY CHECK (length(key) = 32),
        document_id INTEGER NOT NULL, line INTEGER NOT NULL,
        FOREIGN KEY (document_id, line) REFERENCES chunks(document_id, line) ON DELETE CASCADE
    ) STRICT, WITHOUT ROWID;
    CREATE TABLE history (
        id INTEGER PRIMARY KEY,
        time_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
        question TEXT NOT NULL, library_id TEXT NOT NULL, revision INTEGER NOT NULL,
        processing INTEGER NOT NULL CHECK (processing IN (0, 1)), model_id TEXT NOT NULL,
        parameters TEXT NOT NULL, response_format TEXT NOT NULL, endpoint TEXT NOT NULL, credential_scope TEXT NOT NULL,
        from_cache INTEGER NOT NULL CHECK (from_cache IN (0, 1)),
        document_id INTEGER, source TEXT, line INTEGER, quote TEXT, page INTEGER,
        docx_part TEXT, docx_paragraph INTEGER, docx_table INTEGER, docx_row INTEGER, docx_cell INTEGER,
        CHECK ((document_id IS NULL AND source IS NULL AND line IS NULL AND quote IS NULL AND from_cache = 0)
            OR (document_id IS NOT NULL AND source IS NOT NULL AND line IS NOT NULL AND quote IS NOT NULL))
    ) STRICT;
    PRAGMA user_version = 5;
)sql";

using namespace sql;

constexpr auto addSourceBlocks = R"sql(
    CREATE TABLE source_blocks (
        document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
        line INTEGER NOT NULL CHECK (line > 0), text TEXT NOT NULL, page INTEGER,
        docx_part TEXT, docx_paragraph INTEGER, docx_table INTEGER, docx_row INTEGER, docx_cell INTEGER,
        PRIMARY KEY(document_id,line)
    ) STRICT, WITHOUT ROWID;
    INSERT INTO source_blocks SELECT document_id,line,text,page,docx_part,docx_paragraph,docx_table,docx_row,docx_cell FROM chunks;
    ALTER TABLE chunks ADD COLUMN source_line INTEGER CHECK(source_line > 0);
    ALTER TABLE chunks ADD COLUMN first_char INTEGER CHECK(first_char > 0);
    ALTER TABLE chunks ADD COLUMN last_char INTEGER CHECK(last_char >= first_char);
    ALTER TABLE history ADD COLUMN first_char INTEGER CHECK(first_char > 0);
    ALTER TABLE history ADD COLUMN last_char INTEGER CHECK(last_char >= first_char);
    UPDATE search_profile SET settings = settings || '/blocks400-60-v1' WHERE dimension = 0;
    PRAGMA user_version=7;
)sql";

// The publisher now indexes only complete unicode61 terms from each source block.
constexpr auto indexWholeTerms = R"sql(
    DROP TRIGGER index_chunk;
    UPDATE search_profile SET settings=replace(settings,'/blocks400-60-v1','/blocks400-60-v2') WHERE dimension=0;
    PRAGMA user_version=8;
)sql";

constexpr auto addStructuredAnswers = R"sql(
    DROP TABLE answer_cache;
    CREATE TABLE answer_cache (
        id INTEGER PRIMARY KEY,
        key BLOB NOT NULL UNIQUE CHECK (length(key) = 32),
        answered INTEGER NOT NULL CHECK (answered IN (0, 1))
    ) STRICT;
    CREATE TABLE cached_claims (
        cache_id INTEGER NOT NULL REFERENCES answer_cache(id) ON DELETE CASCADE,
        ordinal INTEGER NOT NULL CHECK (ordinal > 0),
        claim TEXT NOT NULL,
        document_id INTEGER NOT NULL CHECK (document_id > 0),
        chunk_id INTEGER NOT NULL CHECK (chunk_id > 0),
        quote TEXT NOT NULL,
        PRIMARY KEY (cache_id, ordinal)
    ) STRICT, WITHOUT ROWID;

    ALTER TABLE history RENAME TO history_legacy;
    CREATE TABLE history (
        id INTEGER PRIMARY KEY,
        time_utc TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
        question TEXT NOT NULL, library_id TEXT NOT NULL, revision INTEGER NOT NULL,
        processing INTEGER NOT NULL CHECK (processing IN (0, 1)), model_id TEXT NOT NULL,
        parameters TEXT NOT NULL, response_format TEXT NOT NULL, endpoint TEXT NOT NULL, credential_scope TEXT NOT NULL,
        from_cache INTEGER NOT NULL CHECK (from_cache IN (0, 1))
    ) STRICT;
    CREATE TABLE history_claims (
        history_id INTEGER NOT NULL REFERENCES history(id) ON DELETE CASCADE,
        ordinal INTEGER NOT NULL CHECK (ordinal > 0),
        claim TEXT NOT NULL,
        document_id INTEGER NOT NULL, source TEXT NOT NULL, line INTEGER NOT NULL, quote TEXT NOT NULL, page INTEGER,
        docx_part TEXT, docx_paragraph INTEGER, docx_table INTEGER, docx_row INTEGER, docx_cell INTEGER,
        first_char INTEGER, last_char INTEGER,
        PRIMARY KEY (history_id, ordinal)
    ) STRICT, WITHOUT ROWID;
    INSERT INTO history
        SELECT id, time_utc, question, library_id, revision, processing, model_id, parameters,
               response_format, endpoint, credential_scope, from_cache FROM history_legacy;
    INSERT INTO history_claims
        SELECT id, 1, quote, document_id, source, line, quote, page, docx_part, docx_paragraph,
               docx_table, docx_row, docx_cell, first_char, last_char
        FROM history_legacy WHERE document_id IS NOT NULL;
    DROP TABLE history_legacy;
    PRAGMA user_version=9;
)sql";

// Rebuild the parent table to replace schema 9's UNIQUE(content, position_layout).
// Child rows keep their original document IDs, including cached and historical citations.
constexpr auto addManagedSources = R"sql(
    CREATE TABLE documents_v10 (
        id INTEGER PRIMARY KEY, source TEXT NOT NULL, content TEXT NOT NULL COLLATE BINARY,
        policy INTEGER NOT NULL CHECK(policy IN (0,1)), position_layout TEXT NOT NULL,
        sha256 TEXT UNIQUE CHECK(length(sha256)=64 AND sha256 NOT GLOB '*[^0-9a-f]*'),
        since_revision INTEGER NOT NULL CHECK(since_revision>=0)
    ) STRICT;
    INSERT INTO documents_v10 SELECT id, source, content, policy, position_layout, NULL, 0 FROM documents;
    DROP TABLE documents;
    ALTER TABLE documents_v10 RENAME TO documents;
    CREATE UNIQUE INDEX text_only_identity ON documents(content,position_layout) WHERE sha256 IS NULL;
    PRAGMA user_version=10;
)sql";

constexpr auto addDocumentLifecycle = R"sql(
    CREATE TABLE documents_v11(
        id INTEGER PRIMARY KEY AUTOINCREMENT, source TEXT NOT NULL, content TEXT NOT NULL COLLATE BINARY,
        policy INTEGER NOT NULL CHECK(policy IN (0,1)), position_layout TEXT NOT NULL,
        sha256 TEXT CHECK(length(sha256)=64 AND sha256 NOT GLOB '*[^0-9a-f]*'),
        since_revision INTEGER NOT NULL CHECK(since_revision>=0),
        retired_revision INTEGER, family_id INTEGER NOT NULL DEFAULT 0
    ) STRICT;
    INSERT INTO documents_v11 SELECT id,source,content,policy,position_layout,sha256,since_revision,NULL,id FROM documents;
    DROP TABLE documents;
    ALTER TABLE documents_v11 RENAME TO documents;
    CREATE UNIQUE INDEX active_digest ON documents(sha256) WHERE retired_revision IS NULL;
    CREATE UNIQUE INDEX text_only_identity ON documents(content,position_layout)
        WHERE sha256 IS NULL AND retired_revision IS NULL;
    CREATE TABLE history_context(
        owner_id INTEGER NOT NULL REFERENCES history(id) ON DELETE CASCADE,
        document_id INTEGER NOT NULL CHECK(document_id>=0),
        PRIMARY KEY(owner_id,document_id)
    ) STRICT, WITHOUT ROWID;
    CREATE TABLE cache_context(
        owner_id INTEGER NOT NULL REFERENCES answer_cache(id) ON DELETE CASCADE,
        document_id INTEGER NOT NULL CHECK(document_id>=0),
        PRIMARY KEY(owner_id,document_id)
    ) STRICT, WITHOUT ROWID;
    PRAGMA user_version=11;
)sql";

struct Schema {
    int application;
    int version;
    bool hasObjects;

    bool supported() const { return application == applicationId && version == schemaVersion; }
    bool migratable() const { return application == applicationId && version >= 1 && version < schemaVersion; }
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
        if (!execute(database, createV2Schema) || !execute(database, addDocxPositions) ||
            !execute(database, addLibraryMetadata) || !execute(database, addCacheAndHistory) ||
            !execute(database, createSearchIndex) || !execute(database, addSourceBlocks) ||
            !execute(database, indexWholeTerms) || !execute(database, addStructuredAnswers) ||
            !execute(database, addManagedSources) || !execute(database, addDocumentLifecycle)) {
            return databaseError(database, AppError::StoreWriteFailed);
        }
    }
    if (!transaction.commit()) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    return std::nullopt;
}

std::optional<AppError> checkIntegrity(sqlite3* database) {
    const auto check = prepare(database, "PRAGMA quick_check(1)");
    if (!check || sqlite3_step(check.get()) != SQLITE_ROW) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    if (readText(check.get(), 0) != "ok") {
        return AppError::StoreCorrupt;
    }
    return std::nullopt;
}

bool backupLegacy(const std::filesystem::path& path, const std::string& filename, int version) {
    // Reserve a directory atomically so neither an old backup nor an existing file is replaced.
    std::filesystem::path directory;
    std::error_code error;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto candidate = path;
        candidate += ".v" + std::to_string(version) + "-backup";
        if (attempt != 0) {
            candidate += "-" + std::to_string(attempt);
        }
        if (std::filesystem::create_directory(candidate, error)) {
            directory = std::move(candidate);
            break;
        }
        if (error && error != std::errc::file_exists) {
            return false;
        }
    }
    if (directory.empty()) {
        return false;
    }

    const auto temporary = directory / "partial.sqlite";
    const auto backupName = encodeUtf8(temporary.native());
    if (!backupName) {
        return false;
    }

    using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
    // The writer holds BEGIN IMMEDIATE. The backup reads the committed legacy image
    // over a separate connection because SQLite cannot back up its own write connection.
    sqlite3* raw = nullptr;
    const auto sourceStatus = sqlite3_open_v2(filename.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    const Connection source(raw, sqlite3_close);
    if (sourceStatus != SQLITE_OK) {
        return false;
    }
    raw = nullptr;
    const auto destinationStatus = sqlite3_open_v2(backupName->c_str(), &raw,
                                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                                    nullptr);
    Connection destination(raw, sqlite3_close);
    if (destinationStatus != SQLITE_OK) {
        return false;
    }
    auto* backup = sqlite3_backup_init(destination.get(), "main", source.get(), "main");
    if (!backup) {
        return false;
    }
    const auto step = sqlite3_backup_step(backup, -1);
    const auto finish = sqlite3_backup_finish(backup);
    if (step != SQLITE_DONE || finish != SQLITE_OK || checkIntegrity(destination.get())) {
        return false;
    }
    destination.reset();
    std::filesystem::rename(temporary, directory / "library.sqlite", error);
    return !error;
}

bool upgradeV1(sqlite3* database) {
    return execute(database, R"sql(
        ALTER TABLE documents RENAME TO documents_v1;
        ALTER TABLE chunks RENAME TO chunks_v1;
    )sql") && execute(database, createV2Schema) && execute(database, R"sql(
        INSERT INTO documents (id, source, content, policy, page_layout)
            SELECT id, source, content, policy, '' FROM documents_v1;
        INSERT INTO chunks (document_id, line, text)
            SELECT document_id, line, text FROM chunks_v1;
        DROP TABLE chunks_v1;
        DROP TABLE documents_v1;
    )sql");
}

std::optional<AppError> migrateLegacy(sqlite3* database, const std::filesystem::path& path,
                                      const std::string& filename) {
    WriteTransaction transaction(database);
    if (!transaction.started()) {
        return databaseError(database, AppError::StoreMigrationFailed);
    }
    const auto schema = readSchema(database);
    if (!schema) {
        return databaseError(database, AppError::StoreMigrationFailed);
    }
    // Another process may have migrated the library while this connection waited for the lock.
    if (!schema->supported()) {
        if (!schema->migratable()) {
            return AppError::StoreSchemaUnsupported;
        }
        if (const auto error = checkIntegrity(database)) {
            return error;
        }
        if (!backupLegacy(path, filename, schema->version) ||
            (schema->version == 1 && !upgradeV1(database)) ||
            (schema->version <= 2 && !execute(database, addDocxPositions)) ||
            (schema->version <= 3 && !execute(database, addLibraryMetadata)) ||
            (schema->version <= 4 && !execute(database, addCacheAndHistory)) ||
            (schema->version <= 5 && !execute(database, createSearchIndex)) ||
            (schema->version <= 6 && !execute(database, addSourceBlocks)) ||
            (schema->version <= 7 && !execute(database, indexWholeTerms)) ||
            (schema->version <= 8 && !execute(database, addStructuredAnswers)) ||
            (schema->version <= 9 && !execute(database, addManagedSources)) ||
            !execute(database, addDocumentLifecycle) ||
            (schema->version >= 4 && schema->version <= 7 &&
             !execute(database, "UPDATE library SET revision = revision + 1 WHERE id=1"))) {
            return databaseError(database, AppError::StoreMigrationFailed);
        }
        if (schema->version <= 7) {
            auto profile = prepare(database, "SELECT dimension FROM search_profile WHERE id=1");
            if (!profile || sqlite3_step(profile.get()) != SQLITE_ROW) return AppError::StoreMigrationFailed;
            // A legacy hybrid index keeps its source data and requires --reindex with its GGUF.
            if (sqlite3_column_int64(profile.get(), 0) == 0 &&
                std::holds_alternative<AppError>(rebuildSearchChunks(database, nullptr))) {
                return AppError::StoreMigrationFailed;
            }
        }
    }
    const auto foreignKeys = prepare(database, "PRAGMA foreign_key_check");
    if (!foreignKeys || sqlite3_step(foreignKeys.get()) != SQLITE_DONE) return AppError::StoreMigrationFailed;
    if (!transaction.commit()) {
        return databaseError(database, AppError::StoreMigrationFailed);
    }
    return std::nullopt;
}

bool bindPositionNumber(sqlite3_stmt* statement, int column, std::optional<std::size_t> value) {
    if (!value) {
        return sqlite3_bind_null(statement, column) == SQLITE_OK;
    }
    return *value > 0 && std::in_range<sqlite3_int64>(*value) &&
           sqlite3_bind_int64(statement, column, static_cast<sqlite3_int64>(*value)) == SQLITE_OK;
}

bool bindDocxPosition(sqlite3_stmt* statement, int first, const std::optional<DocxPosition>& docx) {
    if (!docx) {
        for (int column = first; column < first + 5; ++column) {
            if (sqlite3_bind_null(statement, column) != SQLITE_OK) {
                return false;
            }
        }
        return true;
    }
    if (!bindText(statement, first, docx->part)) {
        return false;
    }
    if (const auto* paragraph = std::get_if<std::size_t>(&docx->anchor)) {
        return bindPositionNumber(statement, first + 1, *paragraph) && bindPositionNumber(statement, first + 2, {}) &&
               bindPositionNumber(statement, first + 3, {}) && bindPositionNumber(statement, first + 4, {});
    }
    const auto& cell = std::get<TableCell>(docx->anchor);
    return bindPositionNumber(statement, first + 1, {}) && bindPositionNumber(statement, first + 2, cell.table) &&
           bindPositionNumber(statement, first + 3, cell.row) && bindPositionNumber(statement, first + 4, cell.cell);
}

Result<std::size_t> duplicateChunkCount(sqlite3* database, const std::string& content,
                                        const std::string& positionLayout, const std::string& digest) {
    const auto query = prepare(database, R"sql(
        SELECT (SELECT count(*) FROM chunks WHERE document_id = documents.id)
        FROM documents WHERE retired_revision IS NULL AND (sha256 = ?3 OR (sha256 IS NULL AND ?3 = '' AND content = ?1 AND position_layout = ?2))
    )sql");
    if (!query || !bindText(query.get(), 1, content) || !bindText(query.get(), 2, positionLayout) || !bindText(query.get(), 3, digest) ||
        sqlite3_step(query.get()) != SQLITE_ROW) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    const auto count = sqlite3_column_int64(query.get(), 0);
    if (count <= 0 || !std::in_range<std::size_t>(count)) {
        return AppError::StoreCorrupt;
    }
    return static_cast<std::size_t>(count);
}

bool advanceRevision(sqlite3* database) {
    return execute(database, "UPDATE library SET revision = revision + 1 WHERE id = 1") &&
           sqlite3_changes(database) == 1;
}

Result<std::size_t> publishBlocks(sqlite3* database, std::optional<sqlite3_int64> document,
                                  const IEmbeddingProvider* embeddings);

Result<std::size_t> writeDocument(sqlite3* database, const std::string& source,
                                  const std::string& content, std::span<const DocumentLine> lines,
                                  DocumentPolicy policy, const std::string& positionLayout,
                                  const IEmbeddingProvider* embeddings, const std::string& digest = {}) {
    const auto document = prepare(database, R"sql(
        INSERT INTO documents (source, content, policy, position_layout, sha256, since_revision)
        SELECT ?1, ?2, ?3, ?4, nullif(?5,''), revision+1 FROM library WHERE id=1
        ON CONFLICT DO NOTHING
    )sql");
    if (!document || !bindText(document.get(), 1, source) || !bindText(document.get(), 2, content) ||
        sqlite3_bind_int(document.get(), 3, policy == DocumentPolicy::AllowRemote ? 1 : 0) != SQLITE_OK ||
        !bindText(document.get(), 4, positionLayout) || !bindText(document.get(), 5, digest) ||
        sqlite3_step(document.get()) != SQLITE_DONE) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    if (sqlite3_changes(database) == 0) {
        return duplicateChunkCount(database, content, positionLayout, digest);
    }

    const auto id = sqlite3_last_insert_rowid(database);
    const auto family = prepare(database, "UPDATE documents SET family_id=id WHERE id=?1");
    if (!family || sqlite3_bind_int64(family.get(), 1, id) != SQLITE_OK ||
        sqlite3_step(family.get()) != SQLITE_DONE)
        return AppError::StoreWriteFailed;
    const auto chunk = prepare(database, R"sql(
        INSERT INTO source_blocks (document_id, line, text, page, docx_part, docx_paragraph,
                            docx_table, docx_row, docx_cell)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
    )sql");
    if (!chunk || sqlite3_bind_int64(chunk.get(), 1, id) != SQLITE_OK) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    for (const auto& line : lines) {
        if (!std::in_range<sqlite3_int64>(line.number) ||
            !bindPositionNumber(chunk.get(), 4, line.page) || !bindDocxPosition(chunk.get(), 5, line.docx) ||
            sqlite3_bind_int64(chunk.get(), 2, static_cast<sqlite3_int64>(line.number)) != SQLITE_OK ||
            !bindText(chunk.get(), 3, line.text) || sqlite3_step(chunk.get()) != SQLITE_DONE ||
            sqlite3_reset(chunk.get()) != SQLITE_OK) {
            return databaseError(database, AppError::StoreWriteFailed);
        }
    }
    const auto published = publishBlocks(database, id, embeddings);
    if (std::holds_alternative<AppError>(published)) return published;
    if (!advanceRevision(database)) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    return published;
}

// A marker with document_id=0 distinguishes a known empty context from an old
// answer that has no context metadata. Positive IDs are immutable version IDs.
Result<bool> writeContext(sqlite3 *db, const char *sql, sqlite3_int64 owner,
                          const std::optional<std::vector<DocumentId>> &documents) {
    if (!documents) return true;
    auto row = prepare(db, sql);
    if (!row || sqlite3_bind_int64(row.get(), 1, owner) != SQLITE_OK ||
        sqlite3_bind_int64(row.get(), 2, 0) != SQLITE_OK || sqlite3_step(row.get()) != SQLITE_DONE)
        return AppError::StoreWriteFailed;
    for (auto id : *documents) {
        if (sqlite3_reset(row.get()) != SQLITE_OK || !bindPositionNumber(row.get(), 2, id) ||
            sqlite3_step(row.get()) != SQLITE_DONE)
            return AppError::StoreWriteFailed;
    }
    return true;
}

Result<std::optional<std::vector<DocumentId>>> readContext(sqlite3 *db, const char *sql,
                                                           sqlite3_int64 owner) {
    auto row = prepare(db, sql);
    if (!row || sqlite3_bind_int64(row.get(), 1, owner) != SQLITE_OK) return AppError::StoreReadFailed;
    std::optional<std::vector<DocumentId>> ids;
    int status;
    while ((status = sqlite3_step(row.get())) == SQLITE_ROW) {
        const auto raw = sqlite3_column_int64(row.get(), 0);
        if (raw == 0)
            ids.emplace();
        else if (!ids || raw < 0 || !std::in_range<DocumentId>(raw))
            return AppError::StoreCorrupt;
        else
            ids->push_back(static_cast<DocumentId>(raw));
    }
    if (status != SQLITE_DONE) return AppError::StoreReadFailed;
    return ids;
}

Result<bool> changePolicy(sqlite3* database, sqlite3_int64 id, DocumentPolicy policy,
                          const char* readSql, const char* updateSql) {
    WriteTransaction transaction(database);
    if (!transaction.started()) {
        if (sqlite3_get_autocommit(database) == 0) return AppError::LibraryBusy;
        return databaseError(database, AppError::StoreWriteFailed);
    }
    const int requested = policy == DocumentPolicy::AllowRemote ? 1 : 0;
    const auto current = prepare(database, readSql);
    if (!current || sqlite3_bind_int64(current.get(), 1, id) != SQLITE_OK ||
        sqlite3_step(current.get()) != SQLITE_ROW) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    if (sqlite3_column_int(current.get(), 0) == requested) {
        return false;
    }
    const auto update = prepare(database, updateSql);
    if (!update || sqlite3_bind_int64(update.get(), 1, id) != SQLITE_OK ||
        sqlite3_bind_int(update.get(), 2, requested) != SQLITE_OK ||
        sqlite3_step(update.get()) != SQLITE_DONE || sqlite3_changes(database) != 1 ||
        !advanceRevision(database) || !transaction.commit()) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    return true;
}

std::optional<std::size_t> readPositionNumber(sqlite3_stmt* query, int column) {
    const auto value = sqlite3_column_int64(query, column);
    if (value <= 0 || !std::in_range<std::size_t>(value)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(value);
}

// Both chunks and historical citations select line, text, page and DOCX coordinates in this order.
Result<Chunk> readChunk(sqlite3_stmt* query, DocumentId id, std::size_t chunkId = 0, int rangeColumn = -1) {
    const auto line = sqlite3_column_int64(query, 0);
    auto text = readText(query, 1);
    if (!text || line <= 0 || !std::in_range<std::size_t>(line)) {
        return AppError::StoreCorrupt;
    }
    const auto page = readPositionNumber(query, 2);
    if (!page && sqlite3_column_type(query, 2) != SQLITE_NULL) {
        return AppError::StoreCorrupt;
    }
    std::optional<DocxPosition> docx;
    if (sqlite3_column_type(query, 3) != SQLITE_NULL) {
        auto part = readText(query, 3);
        if (!part || part->empty() || page) {
            return AppError::StoreCorrupt;
        }
        if (const auto paragraph = readPositionNumber(query, 4)) {
            docx = DocxPosition{std::move(*part), *paragraph};
        } else {
            const auto table = readPositionNumber(query, 5);
            const auto row = readPositionNumber(query, 6);
            const auto cell = readPositionNumber(query, 7);
            if (!table || !row || !cell) {
                return AppError::StoreCorrupt;
            }
            docx = DocxPosition{std::move(*part), TableCell{*table, *row, *cell}};
        }
    }
    std::optional<CharacterRange> range;
    if (rangeColumn >= 0 && sqlite3_column_type(query, rangeColumn) != SQLITE_NULL) {
        const auto first = readPositionNumber(query, rangeColumn);
        const auto last = readPositionNumber(query, rangeColumn + 1);
        if (!first || !last || *last < *first) return AppError::StoreCorrupt;
        range = CharacterRange{*first, *last};
    }
    return Chunk(id, static_cast<std::size_t>(line), std::move(*text), page, std::move(docx), chunkId, range);
}

Result<std::size_t> publishBlocks(sqlite3* db, std::optional<sqlite3_int64> document,
                                  const IEmbeddingProvider* embeddings) {
    std::string select =
        "SELECT line,text,page,docx_part,docx_paragraph,docx_table,docx_row,docx_cell,document_id "
        "FROM source_blocks WHERE document_id IN (SELECT id FROM documents WHERE retired_revision IS NULL) ";
    if (document) select += "AND document_id=?1 ";
    select += "ORDER BY document_id,line";
    auto rows = prepare(db, select.c_str());
    auto insert = prepare(db, R"sql(
        INSERT INTO chunks (document_id,line,text,page,docx_part,docx_paragraph,docx_table,docx_row,docx_cell,
                            source_line,first_char,last_char) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)
    )sql");
    auto fts = prepare(db, "INSERT INTO chunks_fts(document_id,line,text) VALUES(?1,?2,?3)");
    if (!rows || !insert || !fts || (document && sqlite3_bind_int64(rows.get(), 1, *document) != SQLITE_OK))
        return AppError::StoreWriteFailed;
    std::size_t count = 0, chunkId = 0;
    sqlite3_int64 previousDocument = 0;
    int status;
    while ((status = sqlite3_step(rows.get())) == SQLITE_ROW) {
        const auto id = sqlite3_column_int64(rows.get(), 8);
        const auto source = readChunk(rows.get(), static_cast<DocumentId>(id));
        if (const auto* error = std::get_if<AppError>(&source)) return *error;
        const auto& block = std::get<Chunk>(source);
        const auto slices = splitBlock(block.text(), embeddings, db);
        if (const auto* error = std::get_if<AppError>(&slices)) return *error;
        if (previousDocument != id) chunkId = 0;
        previousDocument = id;
        for (const auto& piece : std::get<std::vector<BlockSlice>>(slices)) {
            chunkId = std::max(chunkId + 1, block.line());
            auto* row = insert.get();
            const auto& range = piece.characters;
            if (sqlite3_bind_int64(row, 1, id) != SQLITE_OK || !bindPositionNumber(row, 2, chunkId) ||
                !bindText(row, 3, piece.text) || !bindPositionNumber(row, 4, block.page()) ||
                !bindDocxPosition(row, 5, block.docx()) || !bindPositionNumber(row, 10, block.line()) ||
                !bindPositionNumber(row, 11, range ? std::optional(range->first) : std::nullopt) ||
                !bindPositionNumber(row, 12, range ? std::optional(range->last) : std::nullopt) ||
                sqlite3_step(row) != SQLITE_DONE || sqlite3_reset(row) != SQLITE_OK) return AppError::StoreWriteFailed;
            if (piece.wholeTerms &&
                (sqlite3_bind_int64(fts.get(), 1, id) != SQLITE_OK || !bindPositionNumber(fts.get(), 2, chunkId) ||
                 !bindText(fts.get(), 3, piece.text) || sqlite3_step(fts.get()) != SQLITE_DONE ||
                 sqlite3_reset(fts.get()) != SQLITE_OK)) return AppError::StoreWriteFailed;
            if (embeddings) {
                const auto indexed = indexVector(db, *embeddings, id, chunkId, piece.text);
                if (const auto* error = std::get_if<AppError>(&indexed)) return *error;
            }
            ++count;
        }
    }
    if (status != SQLITE_DONE) return AppError::StoreReadFailed;
    return count;
}

Result<std::vector<Chunk>> readChunks(sqlite3* database, sqlite3_stmt* query, sqlite3_int64 id) {
    if (sqlite3_reset(query) != SQLITE_OK || sqlite3_bind_int64(query, 1, id) != SQLITE_OK) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    std::vector<Chunk> chunks;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(query)) == SQLITE_ROW) {
        const auto key = readPositionNumber(query, 8);
        if (!key) return AppError::StoreCorrupt;
        auto chunk = readChunk(query, static_cast<DocumentId>(id), *key, 9);
        if (const auto* error = std::get_if<AppError>(&chunk)) {
            return *error;
        }
        chunks.push_back(std::get<Chunk>(std::move(chunk)));
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

Result<bool> rebuildSearchChunks(sqlite3* db, const IEmbeddingProvider* provider) {
    // Foreign keys remove vectors and cache references together with their old chunk IDs.
    if (!sql::execute(db, "DELETE FROM chunks_fts; DELETE FROM chunks")) return AppError::StoreWriteFailed;
    const auto published = publishBlocks(db, {}, provider);
    if (const auto* error = std::get_if<AppError>(&published)) return *error;
    return true;
}

void SqliteKnowledgeStore::CloseDatabase::operator()(sqlite3* database) const noexcept {
    sqlite3_close(database);
}

Result<std::unique_ptr<SqliteKnowledgeStore>> SqliteKnowledgeStore::open(
    const std::filesystem::path& path, bool createIfMissing) {
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
    const int flags = SQLITE_OPEN_READWRITE | (!existed && createIfMissing ? SQLITE_OPEN_CREATE : 0);
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
    if (schema->migratable()) {
        // DROP TABLE would otherwise cascade to the old parent's children.
        if (!execute(raw, "PRAGMA foreign_keys=OFF")) return AppError::StoreMigrationFailed;
        if (const auto error = migrateLegacy(raw, absolutePath, *filename)) {
            return *error;
        }
        if (!execute(raw, "PRAGMA foreign_keys=ON")) return AppError::StoreMigrationFailed;
    } else if (!schema->supported()) {
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

    if (const auto error = checkIntegrity(raw)) {
        return *error;
    }
    auto store = std::unique_ptr<SqliteKnowledgeStore>(new SqliteKnowledgeStore(std::move(database)));
    // A committed removal can outlive a locked file; retry under the writer lock.
    store->cleanupSources();
    return store;
}

Result<std::unique_ptr<SqliteKnowledgeStore>> SqliteKnowledgeStore::openInMemory() {
    sqlite3* raw = nullptr;
    const int status = sqlite3_open(":memory:", &raw);
    Database database(raw);
    if (status != SQLITE_OK || !execute(raw, "PRAGMA foreign_keys = ON")) {
        return AppError::StoreUnavailable;
    }
    if (const auto error = initializeSchema(raw)) {
        return *error;
    }
    return std::unique_ptr<SqliteKnowledgeStore>(new SqliteKnowledgeStore(std::move(database)));
}

Result<std::size_t> SqliteKnowledgeStore::save(std::string source, std::string content,
                                              std::vector<DocumentLine> lines, DocumentPolicy policy,
                                              std::string positionLayout) {
    if (lines.empty()) {
        return AppError::StoreWriteFailed;
    }
    WriteTransaction transaction(database_.get());
    if (!transaction.started()) {
        return databaseError(database_.get(), AppError::StoreWriteFailed);
    }

    const auto matches = searchProfileMatches(database_.get(), embeddings_, settings_);
    if (const auto* error = std::get_if<AppError>(&matches)) {
        return *error;
    }
    if (!std::get<bool>(matches)) {
        return AppError::SearchProfileMismatch;
    }
    const auto result = writeDocument(database_.get(), source, content, lines, policy, positionLayout, embeddings_);
    if (std::holds_alternative<AppError>(result)) {
        return result;
    }
    if (!transaction.commit()) {
        return databaseError(database_.get(), AppError::StoreWriteFailed);
    }
    return result;
}

Result<ImportReport> SqliteKnowledgeStore::importSource(const std::filesystem::path& path, const ParseSource& parse) {
    return importManaged(path, parse, {});
}

Result<ImportReport> SqliteKnowledgeStore::updateSource(DocumentId id, const std::filesystem::path &path,
                                                        const ParseSource &parse, std::stop_token stop) {
    if (!id || !std::in_range<sqlite3_int64>(id)) return AppError::StoreWriteFailed;
    return importManaged(path, parse, id, stop);
}

Result<ImportReport> SqliteKnowledgeStore::importManaged(const std::filesystem::path &path,
                                                         const ParseSource &parse,
                                                         std::optional<DocumentId> replace,
                                                         std::stop_token stop) {
    if (stop.stop_requested()) return AppError::Cancelled;
    auto* db = database_.get();
    const char* filename = sqlite3_db_filename(db, "main");
    if (!filename || !*filename)
        return replace ? Result<ImportReport>{AppError::StoreWriteFailed}
                       : IKnowledgeStore::importSource(path, parse);

    WriteTransaction transaction(db);
    if (!transaction.started()) return databaseError(db, AppError::StoreWriteFailed);
    const auto meta = prepare(db, "SELECT library_id FROM library WHERE id=1");
    if (!meta || sqlite3_step(meta.get()) != SQLITE_ROW) return AppError::StoreReadFailed;
    const auto root = sourceDirectory(db, readText(meta.get(), 0).value_or(""));
    const auto profile = searchProfileMatches(db, embeddings_, settings_);
    if (auto* error = std::get_if<AppError>(&profile)) return *error;
    if (!std::get<bool>(profile)) return AppError::SearchProfileMismatch;

    StagedSource stage;
    const auto copy = stage.copy(path, root);
    if (const auto* error = std::get_if<AppError>(&copy)) return *error;
    auto parsed = parse(stage.path());
    if (stop.stop_requested()) return AppError::Cancelled;
    if (auto* error = std::get_if<AppError>(&parsed)) return *error;
    auto& source = std::get<ParsedSource>(parsed);
    const auto& digest = std::get<std::string>(copy);
    sqlite3_int64 family = 0;
    if (replace) {
        auto current = prepare(
            db, "SELECT family_id,sha256,source FROM documents WHERE id=?1 AND retired_revision IS NULL");
        if (!current || !bindPositionNumber(current.get(), 1, *replace) ||
            sqlite3_step(current.get()) != SQLITE_ROW)
            return AppError::StoreWriteFailed;
        family = sqlite3_column_int64(current.get(), 0);
        if (readText(current.get(), 1) == digest) {
            const auto count = duplicateChunkCount(db, source.content, source.layout, digest);
            if (auto *error = std::get_if<AppError>(&count)) return *error;
            auto target = root / digest;
            const auto ext =
                sourceExtension(decodeUtf8(readText(current.get(), 2).value_or("")).value_or(L""));
            if (ext.empty()) return AppError::StoreCorrupt;
            target += ext;
            if (stop.stop_requested()) return AppError::Cancelled;
            if (!verifiedCopy(target, digest) && !stage.publish(target)) return AppError::StoreWriteFailed;
            if (!transaction.commit()) return databaseError(db, AppError::StoreWriteFailed);
            source.report.chunks = std::get<std::size_t>(count);
            source.report.documentId = *replace;
            return std::move(source.report);
        }
        auto duplicate = prepare(db, "SELECT 1 FROM documents WHERE sha256=?1 AND retired_revision IS NULL");
        if (!duplicate || !bindText(duplicate.get(), 1, digest)) return AppError::StoreWriteFailed;
        if (sqlite3_step(duplicate.get()) != SQLITE_DONE) return AppError::StoreWriteFailed;
        auto retire = prepare(
            db,
            "UPDATE documents SET retired_revision=(SELECT revision+1 FROM library),policy=0 WHERE id=?1");
        auto fts = prepare(db, "DELETE FROM chunks_fts WHERE document_id=?1");
        auto chunks = prepare(db, "DELETE FROM chunks WHERE document_id=?1");
        for (auto *statement : {retire.get(), fts.get(), chunks.get()}) {
            if (!statement || !bindPositionNumber(statement, 1, *replace) ||
                sqlite3_step(statement) != SQLITE_DONE)
                return AppError::StoreWriteFailed;
        }
    }
    const auto name = path.filename().u8string();
    const auto result = writeDocument(db, std::string(name.begin(), name.end()), source.content, source.lines,
                                      DocumentPolicy::LocalOnly, source.layout, embeddings_, digest);
    if (const auto* error = std::get_if<AppError>(&result)) return *error;

    // A duplicate keeps its first display name and format.
    auto stored = prepare(db, "SELECT source,id FROM documents WHERE sha256=?1 AND retired_revision IS NULL");
    if (!stored || !bindText(stored.get(), 1, digest) || sqlite3_step(stored.get()) != SQLITE_ROW)
        return AppError::StoreReadFailed;
    const auto id = readPositionNumber(stored.get(), 1);
    if (!id) return AppError::StoreCorrupt;
    if (replace) {
        const auto group = prepare(db, "UPDATE documents SET family_id=?1 WHERE id=?2");
        if (!group || sqlite3_bind_int64(group.get(), 1, family) != SQLITE_OK ||
            !bindPositionNumber(group.get(), 2, *id) || sqlite3_step(group.get()) != SQLITE_DONE)
            return AppError::StoreWriteFailed;
    }
    const auto ext = sourceExtension(decodeUtf8(readText(stored.get(), 0).value_or("")).value_or(L""));
    if (ext.empty()) return AppError::StoreCorrupt;
    auto target = root / digest;
    target += ext;
    if (stop.stop_requested()) return AppError::Cancelled;
    if (!verifiedCopy(target, digest) && !stage.publish(target)) return AppError::StoreWriteFailed;
    if (stop.stop_requested()) return AppError::Cancelled;
    if (!transaction.commit()) return databaseError(db, AppError::StoreWriteFailed);
    source.report.chunks = std::get<std::size_t>(result);
    source.report.documentId = *id;
    return std::move(source.report);
}

Result<CitationSource> SqliteKnowledgeStore::resolveSource(const IndexRevision& revision, const Citation& citation) const {
    auto* db = database_.get();
    const auto row = prepare(db, R"sql(
        SELECT b.line,b.text,b.page,b.docx_part,b.docx_paragraph,b.docx_table,b.docx_row,b.docx_cell,
               d.sha256,d.source
        FROM source_blocks b JOIN documents d ON d.id=b.document_id CROSS JOIN library l
        WHERE l.id=1 AND l.library_id=?1 AND d.id=?2 AND b.line=?3 AND ?4 BETWEEN d.since_revision AND l.revision AND (d.retired_revision IS NULL OR ?4<d.retired_revision)
    )sql");
    if (!std::in_range<sqlite3_int64>(revision.number) || !row ||
        !bindText(row.get(), 1, revision.libraryId) || !bindPositionNumber(row.get(), 2, citation.documentId()) ||
        !bindPositionNumber(row.get(), 3, citation.line()) ||
        sqlite3_bind_int64(row.get(), 4, static_cast<sqlite3_int64>(revision.number)) != SQLITE_OK)
        return AppError::InvalidCitation;
    const int status = sqlite3_step(row.get());
    if (status == SQLITE_DONE) return AppError::InvalidCitation;
    if (status != SQLITE_ROW) return databaseError(db, AppError::StoreReadFailed);
    auto block = readChunk(row.get(), citation.documentId());
    if (const auto* error = std::get_if<AppError>(&block)) return *error;
    const auto& chunk = std::get<Chunk>(block);
    if (chunk.page() != citation.page() || chunk.docx() != citation.docx() ||
        readText(row.get(), 9) != citation.source() || citation.quote().empty()) return AppError::InvalidCitation;

    const auto highlight = locateQuote(chunk.text(), citation);
    if (const auto* error = std::get_if<AppError>(&highlight)) return *error;
    CitationSource source{chunk.text(), std::get<CharacterRange>(highlight), CopyStatus::Legacy, {}, {}};
    const auto digest = readText(row.get(), 8);
    if (!digest) return source;
    const auto root = sourceDirectory(db, revision.libraryId);
    const auto ext = sourceExtension(decodeUtf8(citation.source()).value_or(L""));
    if (!validDigest(*digest) || root.empty() || ext.empty()) return AppError::StoreCorrupt;
    auto path = root / *digest;
    path += ext;
    source.sha256 = *digest;
    source.copyStatus = CopyStatus::Unavailable;
    if (verifiedCopy(path, *digest)) {
        source.copyPath = std::move(path);
        source.copyStatus = CopyStatus::Available;
    }
    return source;
}

namespace {
constexpr auto unknownCountsSql = R"sql(
    SELECT
      (SELECT count(*) FROM history h WHERE NOT EXISTS(SELECT 1 FROM history_context c WHERE c.owner_id=h.id AND c.document_id=0)),
      (SELECT count(*) FROM answer_cache a WHERE NOT EXISTS(SELECT 1 FROM cache_context c WHERE c.owner_id=a.id AND c.document_id=0))
)sql";

Result<ContextCounts> contextCounts(sqlite3 *db, const char *sql, std::optional<DocumentId> id = {}) {
    auto row = prepare(db, sql);
    if (!row || (id && !bindPositionNumber(row.get(), 1, *id)) || sqlite3_step(row.get()) != SQLITE_ROW)
        return AppError::StoreReadFailed;
    return ContextCounts{static_cast<std::size_t>(sqlite3_column_int64(row.get(), 0)),
                         static_cast<std::size_t>(sqlite3_column_int64(row.get(), 1))};
}
} // namespace

Result<DeletionPlan> SqliteKnowledgeStore::deletionPlan(DocumentId id) const {
    auto *db = database_.get();
    auto current = prepare(db, "SELECT family_id FROM documents WHERE id=?1 AND retired_revision IS NULL");
    if (!current || !bindPositionNumber(current.get(), 1, id) || sqlite3_step(current.get()) != SQLITE_ROW)
        return AppError::StoreReadFailed;
    auto family = readPositionNumber(current.get(), 0);
    if (!family) return AppError::StoreCorrupt;
    const auto unknown = contextCounts(db, unknownCountsSql);
    const auto linked = contextCounts(db, R"sql(
        SELECT
          (SELECT count(DISTINCT owner_id) FROM history_context WHERE document_id IN (SELECT id FROM documents WHERE family_id=?1)),
          (SELECT count(DISTINCT owner_id) FROM cache_context WHERE document_id IN (SELECT id FROM documents WHERE family_id=?1))
    )sql",
                                      *family);
    if (auto *error = std::get_if<AppError>(&unknown)) return *error;
    if (auto *error = std::get_if<AppError>(&linked)) return *error;
    return DeletionPlan{id, std::get<ContextCounts>(linked), std::get<ContextCounts>(unknown)};
}

Result<ContextCounts> SqliteKnowledgeStore::unknownContextCounts() const {
    return contextCounts(database_.get(), unknownCountsSql);
}

Result<ContextCounts> SqliteKnowledgeStore::clearUnknownContext() {
    auto *db = database_.get();
    WriteTransaction transaction(db);
    if (!transaction.started()) return databaseError(db, AppError::StoreWriteFailed);
    auto count = contextCounts(db, unknownCountsSql);
    if (auto *error = std::get_if<AppError>(&count)) return *error;
    if (!execute(db, R"sql(
        DELETE FROM history WHERE NOT EXISTS(SELECT 1 FROM history_context c WHERE c.owner_id=history.id AND c.document_id=0);
        DELETE FROM answer_cache WHERE NOT EXISTS(SELECT 1 FROM cache_context c WHERE c.owner_id=answer_cache.id AND c.document_id=0);
    )sql") ||
        !transaction.commit())
        return databaseError(db, AppError::StoreWriteFailed);
    return count;
}

Result<DeletionReport> SqliteKnowledgeStore::deleteDocument(DocumentId id) {
    auto *db = database_.get();
    WriteTransaction transaction(db);
    if (!transaction.started()) return databaseError(db, AppError::StoreWriteFailed);
    auto preview = deletionPlan(id);
    if (auto *error = std::get_if<AppError>(&preview)) return *error;
    const auto &plan = std::get<DeletionPlan>(preview);
    if (plan.unknown.history || plan.unknown.cache) return DeletionReport{false, plan.unknown, 0};
    auto current = prepare(db, "SELECT family_id FROM documents WHERE id=?1 AND retired_revision IS NULL");
    if (!current || !bindPositionNumber(current.get(), 1, id) || sqlite3_step(current.get()) != SQLITE_ROW)
        return AppError::StoreReadFailed;
    const auto family = sqlite3_column_int64(current.get(), 0);
    current.reset();
    for (auto sql :
         {"DELETE FROM history WHERE id IN (SELECT owner_id FROM history_context WHERE document_id IN "
          "(SELECT id FROM documents WHERE family_id=?1))",
          "DELETE FROM answer_cache WHERE id IN (SELECT owner_id FROM cache_context WHERE document_id IN "
          "(SELECT id FROM documents WHERE family_id=?1))",
          "DELETE FROM chunks_fts WHERE document_id IN (SELECT id FROM documents WHERE family_id=?1)",
          "DELETE FROM documents WHERE family_id=?1"}) {
        auto row = prepare(db, sql);
        if (!row || sqlite3_bind_int64(row.get(), 1, family) != SQLITE_OK ||
            sqlite3_step(row.get()) != SQLITE_DONE)
            return databaseError(db, AppError::StoreWriteFailed);
    }
    if (!advanceRevision(db) || !transaction.commit()) return databaseError(db, AppError::StoreWriteFailed);
    const auto cleaned = cleanupSources();
    return DeletionReport{
        true, {}, std::holds_alternative<AppError>(cleaned) ? 1 : std::get<std::size_t>(cleaned)};
}

Result<std::size_t> SqliteKnowledgeStore::cleanupSources() {
    auto *db = database_.get();
    WriteTransaction transaction(db);
    if (!transaction.started()) return databaseError(db, AppError::StoreWriteFailed);
    auto meta = prepare(db, "SELECT library_id FROM library WHERE id=1");
    if (!meta || sqlite3_step(meta.get()) != SQLITE_ROW) return AppError::StoreReadFailed;
    auto root = sourceDirectory(db, readText(meta.get(), 0).value_or(""));
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root, ec))
        return ec ? Result<std::size_t>{AppError::StoreReadFailed} : Result<std::size_t>{std::size_t{0}};
    if (!isLocalStoragePath(root)) return AppError::StoreWriteFailed;
    auto refs = prepare(db, "SELECT source FROM documents WHERE sha256=?1");
    if (!refs) return AppError::StoreReadFailed;
    std::size_t pending = 0;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path();
        const auto extension = sourceExtension(path);
        const auto utf8 = path.stem().u8string();
        const std::string stem(utf8.begin(), utf8.end());
        if (extension.empty()) continue;
        const bool partial = stem.starts_with("partial-") && validDigest(std::string_view(stem).substr(8));
        if (!partial && !validDigest(stem)) continue;
        bool referenced = false;
        if (!partial) {
            if (sqlite3_reset(refs.get()) != SQLITE_OK || !bindText(refs.get(), 1, stem))
                return AppError::StoreReadFailed;
            int status;
            while ((status = sqlite3_step(refs.get())) == SQLITE_ROW) {
                if (sourceExtension(decodeUtf8(readText(refs.get(), 0).value_or("")).value_or(L"")) ==
                    extension)
                    referenced = true;
            }
            if (status != SQLITE_DONE) return AppError::StoreReadFailed;
        }
        if (referenced) continue;
        std::error_code removal;
        if (!isLocalStoragePath(path) || !std::filesystem::is_regular_file(path, removal) ||
            !std::filesystem::remove(path, removal) || removal)
            ++pending;
    }
    if (ec) return AppError::StoreWriteFailed;
    if (!transaction.commit()) return AppError::StoreWriteFailed;
    return pending;
}

Result<bool> SqliteKnowledgeStore::setLibraryPolicy(DocumentPolicy policy) {
    return changePolicy(database_.get(), 1, policy, "SELECT policy FROM library WHERE id = ?1",
                        "UPDATE library SET policy = ?2 WHERE id = ?1");
}

Result<bool> SqliteKnowledgeStore::setDocumentPolicy(DocumentId id, DocumentPolicy policy) {
    if (!std::in_range<sqlite3_int64>(id)) {
        return AppError::StoreWriteFailed;
    }
    return changePolicy(database_.get(), static_cast<sqlite3_int64>(id), policy,
                        "SELECT policy FROM documents WHERE id = ?1 AND retired_revision IS NULL",
                        "UPDATE documents SET policy = ?2 WHERE id = ?1 AND retired_revision IS NULL");
}

Result<Answer> SqliteKnowledgeStore::runRemoteQuestion(const RemoteQuestion& question) const {
    auto* db = database_.get();
    // Reserve SQLite's writer slot before reading any policy, including with WAL enabled.
    WriteTransaction transaction(db);
    if (!transaction.started()) {
        if (sqlite3_get_autocommit(db) == 0) return AppError::LibraryBusy;
        return databaseError(db, AppError::StoreWriteFailed);
    }
    auto result = question();
    if (std::holds_alternative<Answer>(result) && !transaction.commit()) {
        return databaseError(db, AppError::StoreWriteFailed);
    }
    return result;
}

Result<LibrarySnapshot> SqliteKnowledgeStore::snapshot() const {
    auto* database = database_.get();
    // Keep this SELECT on its row until all documents and chunks have been copied.
    // The active cursor pins SQLite's read transaction, including for an empty library.
    const auto metadata = prepare(database, "SELECT library_id, revision, policy FROM library WHERE id = 1");
    if (!metadata) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    const auto metadataStatus = sqlite3_step(metadata.get());
    if (metadataStatus != SQLITE_ROW) {
        return metadataStatus == SQLITE_DONE ? AppError::StoreCorrupt :
                                               databaseError(database, AppError::StoreReadFailed);
    }
    auto libraryId = readText(metadata.get(), 0);
    const auto number = sqlite3_column_int64(metadata.get(), 1);
    const auto libraryPolicy = sqlite3_column_int(metadata.get(), 2);
    if (!libraryId || libraryId->size() != 32 || number < 0 || (libraryPolicy != 0 && libraryPolicy != 1)) {
        return AppError::StoreCorrupt;
    }
    LibrarySnapshot result{{std::move(*libraryId), static_cast<std::uint64_t>(number)},
                           libraryPolicy == 1 ? DocumentPolicy::AllowRemote : DocumentPolicy::LocalOnly, {}};

    const auto documents = prepare(database, "SELECT id, source, content, policy, position_layout FROM "
                                             "documents WHERE retired_revision IS NULL ORDER BY id");
    const auto chunks = prepare(database, R"sql(
        SELECT coalesce(source_line,line), text, page, docx_part, docx_paragraph, docx_table, docx_row, docx_cell,
               line, first_char, last_char
        FROM chunks WHERE document_id = ?1 ORDER BY line
    )sql");
    if (!documents || !chunks) {
        return databaseError(database, AppError::StoreReadFailed);
    }

    int status = SQLITE_OK;
    while ((status = sqlite3_step(documents.get())) == SQLITE_ROW) {
        const auto id = sqlite3_column_int64(documents.get(), 0);
        auto source = readText(documents.get(), 1);
        auto content = readText(documents.get(), 2);
        const auto policy = sqlite3_column_int(documents.get(), 3);
        auto positionLayout = readText(documents.get(), 4);
        if (!source || !content || !positionLayout || id <= 0 || !std::in_range<DocumentId>(id) ||
            (policy != 0 && policy != 1)) {
            return AppError::StoreCorrupt;
        }
        auto storedChunks = readChunks(database, chunks.get(), id);
        if (const auto* error = std::get_if<AppError>(&storedChunks)) {
            return *error;
        }
        result.documents.emplace_back(static_cast<DocumentId>(id), std::move(*source), std::move(*content),
                                      std::get<std::vector<Chunk>>(std::move(storedChunks)),
                                      policy == 1 ? DocumentPolicy::AllowRemote : DocumentPolicy::LocalOnly,
                                      std::move(*positionLayout));
    }
    if (status != SQLITE_DONE) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    return result;
}

Result<std::optional<GenerationResult>> SqliteKnowledgeStore::lookup(const GenerationRequest& request) const {
    auto* db = database_.get();
    const auto key = cacheKey(request);
    const auto stmt = prepare(db, "SELECT id, answered FROM answer_cache WHERE key = ?1");
    if (!key || !stmt || sqlite3_bind_blob(stmt.get(), 1, key->data(), static_cast<int>(key->size()),
                                          SQLITE_TRANSIENT) != SQLITE_OK) {
        return databaseError(db, AppError::StoreReadFailed);
    }
    const auto status = sqlite3_step(stmt.get());
    if (status == SQLITE_DONE) return std::optional<GenerationResult>{};
    if (status != SQLITE_ROW) return databaseError(db, AppError::StoreReadFailed);

    const auto rows = prepare(db, R"sql(
        SELECT claim, document_id, chunk_id, quote FROM cached_claims
        WHERE cache_id = ?1 ORDER BY ordinal
    )sql");
    if (!rows || sqlite3_bind_int64(rows.get(), 1, sqlite3_column_int64(stmt.get(), 0)) != SQLITE_OK) {
        return databaseError(db, AppError::StoreReadFailed);
    }
    Answered answered;
    int step;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
        auto claim = readText(rows.get(), 0);
        const auto document = readPositionNumber(rows.get(), 1);
        const auto chunk = readPositionNumber(rows.get(), 2);
        auto quote = readText(rows.get(), 3);
        const auto match = std::ranges::find_if(request.evidence, [&](const Chunk& evidence) {
            return evidence.documentId() == document && evidence.chunkId() == chunk;
        });
        if (!claim || !quote || match == request.evidence.end()) return AppError::StoreCorrupt;
        answered.claims.push_back({std::move(*claim),
            contextChunkId(static_cast<std::size_t>(match - request.evidence.begin())), std::move(*quote)});
    }
    if (step != SQLITE_DONE) return databaseError(db, AppError::StoreReadFailed);

    const bool hasAnswer = sqlite3_column_int(stmt.get(), 1) != 0;
    if (hasAnswer != !answered.claims.empty()) return AppError::StoreCorrupt;
    auto context =
        readContext(db, "SELECT document_id FROM cache_context WHERE owner_id=?1 ORDER BY document_id",
                    sqlite3_column_int64(stmt.get(), 0));
    if (const auto *error = std::get_if<AppError>(&context)) return *error;
    GenerationResult result;
    if (hasAnswer) result.value = std::move(answered);
    result.context = std::get<std::optional<std::vector<DocumentId>>>(std::move(context));
    return std::optional{std::move(result)};
}

Result<bool> SqliteKnowledgeStore::remember(const GenerationRequest& request, const GenerationResult& result) {
    auto* db = database_.get();
    WriteTransaction transaction(db, WriteMode::AllowSavepoint);
    if (!transaction.started()) return databaseError(db, AppError::StoreWriteFailed);
    const auto key = cacheKey(request);
    const auto* answered = std::get_if<Answered>(&result.value);
    const auto stmt = prepare(db, R"sql(
        INSERT INTO answer_cache (key, answered)
        SELECT ?1, ?2 FROM library WHERE id = 1 AND library_id = ?3 AND revision = ?4
        ON CONFLICT(key) DO NOTHING
    )sql");
    if (!key || !stmt || (answered && answered->claims.empty()) ||
        sqlite3_bind_blob(stmt.get(), 1, key->data(), static_cast<int>(key->size()), SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(stmt.get(), 2, answered != nullptr) != SQLITE_OK ||
        !bindText(stmt.get(), 3, request.revision.libraryId) ||
        !std::in_range<sqlite3_int64>(request.revision.number) ||
        sqlite3_bind_int64(stmt.get(), 4, static_cast<sqlite3_int64>(request.revision.number)) != SQLITE_OK ||
        sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return databaseError(db, AppError::StoreWriteFailed);
    }
    const bool inserted = sqlite3_changes(db) != 0;
    if (inserted) {
        const auto context = writeContext(db, "INSERT INTO cache_context VALUES(?1,?2)",
                                          sqlite3_last_insert_rowid(db), result.context);
        if (const auto *error = std::get_if<AppError>(&context)) return *error;
    }
    if (inserted && answered) {
        const auto rows = prepare(db, R"sql(
            INSERT INTO cached_claims (cache_id, ordinal, claim, document_id, chunk_id, quote)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6)
        )sql");
        if (!rows || sqlite3_bind_int64(rows.get(), 1, sqlite3_last_insert_rowid(db)) != SQLITE_OK) {
            return databaseError(db, AppError::StoreWriteFailed);
        }
        std::size_t ordinal = 0;
        for (const auto& claim : answered->claims) {
            const auto index = contextChunkIndex(claim.chunkId, request.evidence.size());
            if (!index) return AppError::StoreWriteFailed;
            const auto& chunk = request.evidence[*index];
            if (!bindPositionNumber(rows.get(), 2, ++ordinal) || !bindText(rows.get(), 3, claim.text) ||
                !bindPositionNumber(rows.get(), 4, chunk.documentId()) ||
                !bindPositionNumber(rows.get(), 5, chunk.chunkId()) || !bindText(rows.get(), 6, claim.quote) ||
                sqlite3_step(rows.get()) != SQLITE_DONE || sqlite3_reset(rows.get()) != SQLITE_OK) {
                return databaseError(db, AppError::StoreWriteFailed);
            }
        }
    }
    if (!transaction.commit()) return databaseError(db, AppError::StoreWriteFailed);
    return inserted;
}

Result<bool> SqliteKnowledgeStore::appendHistory(const std::string& question, const IndexRevision& revision,
                                                const ModelProfile& profile, const Answer& answer) {
    auto* db = database_.get();
    WriteTransaction transaction(db, WriteMode::AllowSavepoint);
    if (!transaction.started()) return databaseError(db, AppError::StoreWriteFailed);
    const auto stmt = prepare(db, R"sql(
        INSERT INTO history (question, library_id, revision, processing, model_id, parameters,
            response_format, endpoint, credential_scope, from_cache)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)
    )sql");
    auto* row = stmt.get();
    if (!row || !bindText(row, 1, question) || !bindText(row, 2, revision.libraryId) ||
        !std::in_range<sqlite3_int64>(revision.number) ||
        sqlite3_bind_int64(row, 3, static_cast<sqlite3_int64>(revision.number)) != SQLITE_OK ||
        sqlite3_bind_int(row, 4, profile.processing == ProcessingType::Remote) != SQLITE_OK ||
        !bindText(row, 5, profile.modelId) || !bindText(row, 6, profile.generationParameters) ||
        !bindText(row, 7, profile.responseFormat) || !bindText(row, 8, profile.endpoint) ||
        !bindText(row, 9, profile.credentialScope) || sqlite3_bind_int(row, 10, answer.fromCache()) != SQLITE_OK ||
        sqlite3_step(row) != SQLITE_DONE) {
        return databaseError(db, AppError::StoreWriteFailed);
    }

    {
        auto current = prepare(db, "SELECT 1 FROM library WHERE id=1 AND library_id=?1 AND revision=?2");
        if (!current || !bindText(current.get(), 1, revision.libraryId) ||
            sqlite3_bind_int64(current.get(), 2, static_cast<sqlite3_int64>(revision.number)) != SQLITE_OK ||
            sqlite3_step(current.get()) != SQLITE_ROW)
            return AppError::LibraryBusy;
    }
    const auto context = writeContext(db, "INSERT INTO history_context VALUES(?1,?2)",
                                      sqlite3_last_insert_rowid(db), answer.context());
    if (const auto *error = std::get_if<AppError>(&context)) return *error;

    if (!answer.claims().empty()) {
        const auto rows = prepare(db, R"sql(
            INSERT INTO history_claims (history_id, ordinal, claim, document_id, source, line, quote, page,
                docx_part, docx_paragraph, docx_table, docx_row, docx_cell, first_char, last_char)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)
        )sql");
        if (!rows || sqlite3_bind_int64(rows.get(), 1, sqlite3_last_insert_rowid(db)) != SQLITE_OK) {
            return databaseError(db, AppError::StoreWriteFailed);
        }
        std::size_t ordinal = 0;
        for (const auto& claim : answer.claims()) {
            const auto& c = claim.citation;
            const auto range = c.characters();
            if (!bindPositionNumber(rows.get(), 2, ++ordinal) || !bindText(rows.get(), 3, claim.text) ||
                !bindPositionNumber(rows.get(), 4, c.documentId()) || !bindText(rows.get(), 5, c.source()) ||
                !bindPositionNumber(rows.get(), 6, c.line()) || !bindText(rows.get(), 7, c.quote()) ||
                !bindPositionNumber(rows.get(), 8, c.page()) || !bindDocxPosition(rows.get(), 9, c.docx()) ||
                !bindPositionNumber(rows.get(), 14, range ? std::optional(range->first) : std::nullopt) ||
                !bindPositionNumber(rows.get(), 15, range ? std::optional(range->last) : std::nullopt) ||
                sqlite3_step(rows.get()) != SQLITE_DONE || sqlite3_reset(rows.get()) != SQLITE_OK) {
                return databaseError(db, AppError::StoreWriteFailed);
            }
        }
    }
    if (!transaction.commit()) return databaseError(db, AppError::StoreWriteFailed);
    return true;
}

Result<std::vector<HistoryEntry>> SqliteKnowledgeStore::history() const {
    auto* db = database_.get();
    const auto stmt = prepare(db, R"sql(
        SELECT h.id, h.from_cache, h.time_utc, h.question, h.library_id, h.revision,
               h.processing, h.model_id, h.parameters, h.response_format, h.endpoint, h.credential_scope,
               h.library_id <> lib.library_id OR h.revision <> lib.revision
        FROM history h CROSS JOIN library lib WHERE lib.id = 1 ORDER BY h.id
    )sql");
    const auto rows = prepare(db, R"sql(
        SELECT line, quote, page, docx_part, docx_paragraph, docx_table, docx_row, docx_cell,
               first_char, last_char, document_id, source, claim
        FROM history_claims WHERE history_id = ?1 ORDER BY ordinal
    )sql");
    auto* row = stmt.get();
    if (!row || !rows) return databaseError(db, AppError::StoreReadFailed);

    std::vector<HistoryEntry> entries;
    int status;
    while ((status = sqlite3_step(row)) == SQLITE_ROW) {
        if (sqlite3_bind_int64(rows.get(), 1, sqlite3_column_int64(row, 0)) != SQLITE_OK) {
            return databaseError(db, AppError::StoreReadFailed);
        }
        std::vector<CitedClaim> claims;
        int step;
        while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
            const auto id = readPositionNumber(rows.get(), 10);
            auto source = readText(rows.get(), 11);
            auto text = readText(rows.get(), 12);
            if (!id || !source || !text) return AppError::StoreCorrupt;
            auto chunk = readChunk(rows.get(), *id, 0, 8);
            if (const auto* error = std::get_if<AppError>(&chunk)) return *error;
            claims.push_back({std::move(*text), Citation(std::move(*source), std::get<Chunk>(std::move(chunk)))});
        }
        if (step != SQLITE_DONE || sqlite3_reset(rows.get()) != SQLITE_OK) {
            return databaseError(db, AppError::StoreReadFailed);
        }

        auto time = readText(row, 2);
        auto question = readText(row, 3);
        auto library = readText(row, 4);
        const auto revision = sqlite3_column_int64(row, 5);
        auto model = readText(row, 7);
        auto parameters = readText(row, 8);
        auto format = readText(row, 9);
        auto endpoint = readText(row, 10);
        auto scope = readText(row, 11);
        if (!time || !question || !library || revision < 0 || !model || !parameters || !format || !endpoint || !scope) {
            return AppError::StoreCorrupt;
        }
        auto context =
            readContext(db, "SELECT document_id FROM history_context WHERE owner_id=?1 ORDER BY document_id",
                        sqlite3_column_int64(row, 0));
        if (const auto *error = std::get_if<AppError>(&context)) return *error;
        const auto processing = sqlite3_column_int(row, 6) ? ProcessingType::Remote : ProcessingType::Local;
        entries.push_back({std::move(*time),
                           std::move(*question),
                           {std::move(*library), static_cast<std::uint64_t>(revision)},
                           {processing, std::move(*model), std::move(*parameters), std::move(*format),
                            std::move(*endpoint), std::move(*scope)},
                           Answer(std::move(claims), sqlite3_column_int(row, 1) != 0,
                                  std::get<std::optional<std::vector<DocumentId>>>(std::move(context))),
                           sqlite3_column_int(row, 12) != 0});
    }
    if (status != SQLITE_DONE) return databaseError(db, AppError::StoreReadFailed);
    return entries;
}
Result<bool> SqliteKnowledgeStore::clearHistory() {
    if (!execute(database_.get(), "DELETE FROM history")) {
        return databaseError(database_.get(), AppError::StoreWriteFailed);
    }
    return true;
}

}  // namespace rag
