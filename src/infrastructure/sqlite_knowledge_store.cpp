#include "rag/infrastructure/sqlite_knowledge_store.hpp"
#include "rag/infrastructure/utf8.hpp"
#include "cache_key.hpp"

#include <sqlite3.h>

namespace rag {
namespace {

constexpr int applicationId = 0x52414742;  // RAGB
constexpr int schemaVersion = 5;
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
            !execute(database, addLibraryMetadata) || !execute(database, addCacheAndHistory)) {
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
            !execute(database, addCacheAndHistory)) {
            return databaseError(database, AppError::StoreMigrationFailed);
        }
    }
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
                                        const std::string& positionLayout) {
    const auto query = prepare(database, R"sql(
        SELECT (SELECT count(*) FROM chunks WHERE document_id = documents.id)
        FROM documents WHERE content = ?1 AND position_layout = ?2
    )sql");
    if (!query || !bindText(query.get(), 1, content) || !bindText(query.get(), 2, positionLayout) ||
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

Result<std::size_t> writeDocument(sqlite3* database, const std::string& source,
                                  const std::string& content, std::span<const DocumentLine> lines,
                                  DocumentPolicy policy, const std::string& positionLayout) {
    const auto document = prepare(database, R"sql(
        INSERT INTO documents (source, content, policy, position_layout) VALUES (?1, ?2, ?3, ?4)
        ON CONFLICT (content, position_layout) DO NOTHING
    )sql");
    if (!document || !bindText(document.get(), 1, source) || !bindText(document.get(), 2, content) ||
        sqlite3_bind_int(document.get(), 3, policy == DocumentPolicy::AllowRemote ? 1 : 0) != SQLITE_OK ||
        !bindText(document.get(), 4, positionLayout) ||
        sqlite3_step(document.get()) != SQLITE_DONE) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    if (sqlite3_changes(database) == 0) {
        return duplicateChunkCount(database, content, positionLayout);
    }

    const auto id = sqlite3_last_insert_rowid(database);
    const auto chunk = prepare(database, R"sql(
        INSERT INTO chunks (document_id, line, text, page, docx_part, docx_paragraph,
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
    if (!advanceRevision(database)) {
        return databaseError(database, AppError::StoreWriteFailed);
    }
    return lines.size();
}

Result<bool> changePolicy(sqlite3* database, sqlite3_int64 id, DocumentPolicy policy,
                          const char* readSql, const char* updateSql) {
    WriteTransaction transaction(database);
    if (!transaction.started()) {
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
Result<Chunk> readChunk(sqlite3_stmt* query, DocumentId id) {
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
    return Chunk(id, static_cast<std::size_t>(line), std::move(*text), page, std::move(docx));
}

Result<std::vector<Chunk>> readChunks(sqlite3* database, sqlite3_stmt* query, sqlite3_int64 id) {
    if (sqlite3_reset(query) != SQLITE_OK || sqlite3_bind_int64(query, 1, id) != SQLITE_OK) {
        return databaseError(database, AppError::StoreReadFailed);
    }
    std::vector<Chunk> chunks;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(query)) == SQLITE_ROW) {
        auto chunk = readChunk(query, static_cast<DocumentId>(id));
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
        if (const auto error = migrateLegacy(raw, absolutePath, *filename)) {
            return *error;
        }
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
    return std::unique_ptr<SqliteKnowledgeStore>(new SqliteKnowledgeStore(std::move(database)));
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

    const auto result = writeDocument(database_.get(), source, content, lines, policy, positionLayout);
    if (std::holds_alternative<AppError>(result)) {
        return result;
    }
    if (!transaction.commit()) {
        return databaseError(database_.get(), AppError::StoreWriteFailed);
    }
    return result;
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
                        "SELECT policy FROM documents WHERE id = ?1",
                        "UPDATE documents SET policy = ?2 WHERE id = ?1");
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

    const auto documents = prepare(database,
        "SELECT id, source, content, policy, position_layout FROM documents ORDER BY id");
    const auto chunks = prepare(database, R"sql(
        SELECT line, text, page, docx_part, docx_paragraph, docx_table, docx_row, docx_cell
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

Result<std::optional<ChunkReference>> SqliteKnowledgeStore::lookup(const GenerationRequest& request) const {
    auto* db = database_.get();
    const auto key = cacheKey(request);
    const auto stmt = prepare(db, "SELECT document_id, line FROM answer_cache WHERE key = ?1");
    if (!key || !stmt || sqlite3_bind_blob(stmt.get(), 1, key->data(), static_cast<int>(key->size()),
                                          SQLITE_TRANSIENT) != SQLITE_OK) {
        return databaseError(db, AppError::StoreReadFailed);
    }
    const auto status = sqlite3_step(stmt.get());
    if (status == SQLITE_DONE) {
        return std::optional<ChunkReference>{};
    }
    if (status != SQLITE_ROW) {
        return databaseError(db, AppError::StoreReadFailed);
    }
    const auto id = readPositionNumber(stmt.get(), 0);
    const auto line = readPositionNumber(stmt.get(), 1);
    if (!id || !line) {
        return AppError::StoreCorrupt;
    }
    return std::optional<ChunkReference>{{*id, *line}};
}

Result<bool> SqliteKnowledgeStore::remember(const GenerationRequest& request, ChunkReference citation) {
    auto* db = database_.get();
    const auto key = cacheKey(request);
    const auto stmt = prepare(db, "INSERT INTO answer_cache (key, document_id, line) VALUES (?1, ?2, ?3)"
                                  " ON CONFLICT(key) DO NOTHING");
    if (!key || !stmt || sqlite3_bind_blob(stmt.get(), 1, key->data(), static_cast<int>(key->size()),
                                          SQLITE_TRANSIENT) != SQLITE_OK ||
        !bindPositionNumber(stmt.get(), 2, citation.documentId) || !bindPositionNumber(stmt.get(), 3, citation.line) ||
        sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return databaseError(db, AppError::StoreWriteFailed);
    }
    return true;
}

Result<bool> SqliteKnowledgeStore::appendHistory(const std::string& question, const IndexRevision& revision,
                                                const ModelProfile& profile, const Answer& answer) {
    auto* db = database_.get();
    const auto stmt = prepare(db, R"sql(
        INSERT INTO history (question, library_id, revision, processing, model_id, parameters,
            response_format, endpoint, credential_scope, from_cache, document_id, source, line, quote, page,
            docx_part, docx_paragraph, docx_table, docx_row, docx_cell)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20)
    )sql");
    auto* row = stmt.get();
    if (!row || !bindText(row, 1, question) || !bindText(row, 2, revision.libraryId) ||
        !std::in_range<sqlite3_int64>(revision.number) ||
        sqlite3_bind_int64(row, 3, static_cast<sqlite3_int64>(revision.number)) != SQLITE_OK ||
        sqlite3_bind_int(row, 4, profile.processing == ProcessingType::Remote) != SQLITE_OK ||
        !bindText(row, 5, profile.modelId) || !bindText(row, 6, profile.generationParameters) ||
        !bindText(row, 7, profile.responseFormat) || !bindText(row, 8, profile.endpoint) ||
        !bindText(row, 9, profile.credentialScope) || sqlite3_bind_int(row, 10, answer.fromCache()) != SQLITE_OK) {
        return databaseError(db, AppError::StoreWriteFailed);
    }
    if (const auto& c = answer.citation()) {
        if (!bindPositionNumber(row, 11, c->documentId()) || !bindText(row, 12, c->source()) ||
            !bindPositionNumber(row, 13, c->line()) || !bindText(row, 14, c->quote()) ||
            !bindPositionNumber(row, 15, c->page()) || !bindDocxPosition(row, 16, c->docx())) {
            return databaseError(db, AppError::StoreWriteFailed);
        }
    }
    if (sqlite3_step(row) != SQLITE_DONE) {
        return databaseError(db, AppError::StoreWriteFailed);
    }
    return true;
}

Result<std::vector<HistoryEntry>> SqliteKnowledgeStore::history() const {
    auto* db = database_.get();
    const auto stmt = prepare(db, R"sql(
        SELECT h.line, h.quote, h.page, h.docx_part, h.docx_paragraph, h.docx_table, h.docx_row, h.docx_cell,
               h.document_id, h.source, h.from_cache, h.time_utc, h.question, h.library_id, h.revision,
               h.processing, h.model_id, h.parameters, h.response_format, h.endpoint, h.credential_scope,
               h.library_id <> lib.library_id OR h.revision <> lib.revision
        FROM history h CROSS JOIN library lib WHERE lib.id = 1 ORDER BY h.id
    )sql");
    auto* row = stmt.get();
    if (!row) {
        return databaseError(db, AppError::StoreReadFailed);
    }
    std::vector<HistoryEntry> entries;
    int status;
    while ((status = sqlite3_step(row)) == SQLITE_ROW) {
        Answer answer;
        if (sqlite3_column_type(row, 8) != SQLITE_NULL) {
            const auto id = readPositionNumber(row, 8);
            auto source = readText(row, 9);
            if (!id || !source) {
                return AppError::StoreCorrupt;
            }
            auto chunk = readChunk(row, *id);
            if (const auto* error = std::get_if<AppError>(&chunk)) {
                return *error;
            }
            answer = Answer(Citation(std::move(*source), std::get<Chunk>(std::move(chunk))),
                            sqlite3_column_int(row, 10) != 0);
        }
        auto time = readText(row, 11);
        auto question = readText(row, 12);
        auto library = readText(row, 13);
        const auto revision = sqlite3_column_int64(row, 14);
        auto model = readText(row, 16);
        auto parameters = readText(row, 17);
        auto format = readText(row, 18);
        auto endpoint = readText(row, 19);
        auto scope = readText(row, 20);
        if (!time || !question || !library || revision < 0 || !model || !parameters || !format || !endpoint || !scope) {
            return AppError::StoreCorrupt;
        }
        const auto processing = sqlite3_column_int(row, 15) ? ProcessingType::Remote : ProcessingType::Local;
        entries.push_back({std::move(*time), std::move(*question), {std::move(*library), static_cast<std::uint64_t>(revision)},
                           {processing, std::move(*model), std::move(*parameters), std::move(*format),
                            std::move(*endpoint), std::move(*scope)}, std::move(answer), sqlite3_column_int(row, 21) != 0});
    }
    if (status != SQLITE_DONE) {
        return databaseError(db, AppError::StoreReadFailed);
    }
    return entries;
}

Result<bool> SqliteKnowledgeStore::clearHistory() {
    if (!execute(database_.get(), "DELETE FROM history")) {
        return databaseError(database_.get(), AppError::StoreWriteFailed);
    }
    return true;
}

}  // namespace rag
