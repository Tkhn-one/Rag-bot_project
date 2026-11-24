#include "rag/application/services.hpp"
#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/local_embeddings.hpp"
#include "rag/infrastructure/sqlite_knowledge_store.hpp"
#include "rag/infrastructure/utf8.hpp"

#include <sqlite3.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {

using namespace rag;
namespace fs = std::filesystem;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const fs::path& parent) {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = parent / ("sqlite-test-" + std::to_string(timestamp) + "-" +
                                       std::to_string(attempt));
            if (fs::create_directory(candidate)) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("could not create a separate test directory");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T>
void requireError(const Result<T>& result, AppError expected, const char* message) {
    const auto* error = std::get_if<AppError>(&result);
    require(error && *error == expected, message);
}

template <typename T>
T requireValue(Result<T> result, const char* message) {
    if (const auto* error = std::get_if<AppError>(&result)) {
        throw std::runtime_error(std::string(message) + " (AppError " +
                                 std::to_string(static_cast<int>(*error)) + ")");
    }
    return std::get<T>(std::move(result));
}

void writeFixture(const fs::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    require(static_cast<bool>(file), "could not write fixture");
}

std::string readFixture(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "could not read fixture");
    return std::string(std::istreambuf_iterator<char>(file), {});
}

using FixtureConnection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

FixtureConnection fixtureConnection(const fs::path& path, int flags) {
    const auto filename = encodeUtf8(path.native());
    require(filename.has_value(), "could not encode database fixture path");
    sqlite3* raw = nullptr;
    const auto status = sqlite3_open_v2(filename->c_str(), &raw, flags, nullptr);
    FixtureConnection connection(raw, sqlite3_close);
    require(status == SQLITE_OK, "could not open database fixture");
    return connection;
}

void executeFixtureSql(const fs::path& path, const char* sql) {
    const auto connection = fixtureConnection(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (sqlite3_exec(connection.get(), sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(connection.get()));
    }
}

int fixtureScalar(const fs::path& path, const char* sql) {
    const auto connection = fixtureConnection(path, SQLITE_OPEN_READONLY);
    sqlite3_stmt* raw = nullptr;
    const auto prepared = sqlite3_prepare_v2(connection.get(), sql, -1, &raw, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    require(prepared == SQLITE_OK && sqlite3_step(statement.get()) == SQLITE_ROW,
            "could not read scalar from SQLite fixture");
    return sqlite3_column_int(statement.get(), 0);
}

std::unique_ptr<SqliteKnowledgeStore> openStore(const fs::path& path) {
    return requireValue(SqliteKnowledgeStore::open(path), "opening test library");
}

void requireCitation(const IKnowledgeStore& store, std::string question, DocumentId id,
                     std::string_view source, std::string_view quote) {
    const KeywordRetriever retriever;
    MockProvider provider;
    RagService service(store, retriever, provider);
    const auto answer = requireValue(service.ask(std::move(question)), "answering from SQLite");
    const auto& citation = answer.citation();
    require(citation && citation->source() == source && citation->documentId() == id &&
                citation->line() == 4 && !citation->page() && citation->quote() == quote,
            "the stored citation's source, ID, line or text changed");
}

constexpr auto decisionsQuestion = "Резервное копирование";
constexpr auto decisionsQuote = "Резервное копирование выполняется каждую пятницу.";

void reopen(const fs::path& directory, const fs::path& demo) {
    const auto path = directory / L"библиотека с пробелами.sqlite";
    const auto decisions = demo / "decisions.txt";
    const TxtReader reader;
    const auto content = requireValue(reader.read(decisions), "decoding demo TXT").sections.front().text;
    DocumentId decisionsId = 0;
    IndexRevision savedRevision;
    {
        auto store = openStore(path);
        IndexingService index(reader, *store);
        requireValue(index.import(demo / "contract.txt"), "importing the contract");
        requireValue(index.import(decisions), "importing the decisions");
        const auto snapshot = requireValue(store->snapshot(), "reading imported library");
        savedRevision = snapshot.revision;
        const auto& documents = snapshot.documents;
        require(!savedRevision.libraryId.empty() && savedRevision.number == 2,
                "two imports must advance this library's revision twice");
        require(documents.size() == 2, "expected two documents before reopening");
        decisionsId = documents.back().id();
        require(decisionsId != 0 && decisionsId != documents.front().id(), "documents need unique IDs");
        requireCitation(*store, decisionsQuestion, decisionsId, "decisions.txt", decisionsQuote);
    }

    auto store = openStore(path);
    const auto snapshot = requireValue(store->snapshot(), "reading reopened library");
    require(snapshot.revision == savedRevision && snapshot.policy == DocumentPolicy::LocalOnly,
            "reopening must preserve both the library identity and revision");
    const auto& documents = snapshot.documents;
    require(documents.size() == 2 && documents.back().id() == decisionsId &&
                documents.back().content() == content &&
                documents.back().policy() == DocumentPolicy::LocalOnly,
            "the reopened library must preserve the document ID, decoded text and policy");
    requireCitation(*store, decisionsQuestion, decisionsId, "decisions.txt", decisionsQuote);
}

void duplicate(const fs::path& directory, const fs::path& demo) {
    const auto path = directory / "duplicate.sqlite";
    const auto copy = directory / "renamed.txt";
    const std::string quote = "Резервное копирование 'по пятницам'.";
    const std::string content = "\r\n\r\n\r\n" + quote;
    writeFixture(copy, "\xEF\xBB\xBF" + content);
    DocumentId originalId = 0;
    {
        auto store = openStore(path);
        requireValue(store->save("original's.txt", content, {{4, quote}}, DocumentPolicy::AllowRemote),
                     "saving original content and explicit policy");
        const auto documents = requireValue(store->snapshot(), "reading original document").documents;
        require(documents.size() == 1, "expected only the original document");
        originalId = documents.front().id();
    }
    {
        const TxtReader reader;
        auto store = openStore(path);
        IndexingService index(reader, *store);
        require(requireValue(index.import(copy), "reimporting a renamed TXT with a BOM").chunks == 1,
                "duplicate import must return the original chunk count");
        require(requireValue(store->snapshot(), "reading after duplicate").revision.number == 1,
                "duplicate import must not advance the revision");
        requireValue(index.import(demo / "decisions.txt"), "importing different content");
    }

    auto store = openStore(path);
    const auto documents = requireValue(store->snapshot(), "checking the library after duplicate import").documents;
    require(documents.size() == 2 && documents.front().id() == originalId &&
                documents.front().policy() == DocumentPolicy::AllowRemote &&
                documents.back().id() != originalId && documents.back().policy() == DocumentPolicy::LocalOnly,
            "a duplicate must keep its identity and policy, and new content needs its own ID");
    requireCitation(*store, decisionsQuestion, originalId, "original's.txt", quote);
}

void rollback(const fs::path& directory, const fs::path& demo) {
    const auto path = directory / "rollback.sqlite";
    const auto fixture = directory / "two-lines.txt";
    writeFixture(fixture, "First fragment.\nSecond fragment.");
    const TxtReader reader;
    DocumentId originalId = 0;
    {
        auto store = openStore(path);
        IndexingService index(reader, *store);
        requireValue(index.import(demo / "decisions.txt"), "importing the original document");
        const auto documents = requireValue(store->snapshot(), "reading the original library").documents;
        require(documents.size() == 1, "expected one document before injecting the failure");
        originalId = documents.front().id();
        executeFixtureSql(path, R"sql(
            CREATE TRIGGER fail_second_chunk AFTER INSERT ON chunks
            WHEN NEW.line = 2 BEGIN
                INSERT INTO chunks (document_id, line, text) VALUES (-1, 1, 'Orphan fragment');
            END;
        )sql");
        requireError(index.import(fixture), AppError::StoreWriteFailed,
                     "the second chunk's SQLite failure must reach Application");
        const auto afterFailure = requireValue(store->snapshot(), "reading after failed import");
        require(afterFailure.revision.number == 1 && afterFailure.documents.size() == 1 &&
                    afterFailure.documents.front().id() == originalId,
                "no part of the failed document should be visible");
    }

    auto store = openStore(path);
    const auto afterReopen = requireValue(store->snapshot(), "reading after rollback and reopening");
    require(afterReopen.documents.size() == 1 && afterReopen.revision.number == 1,
            "rollback must survive reopening the library");
    requireCitation(*store, decisionsQuestion, originalId, "decisions.txt", decisionsQuote);

    executeFixtureSql(path, "DROP TRIGGER fail_second_chunk");
    IndexingService index(reader, *store);
    require(requireValue(index.import(fixture), "retrying after rollback").chunks == 2,
            "the retry must publish both chunks");
    const auto afterRetry = requireValue(store->snapshot(), "reading after retry");
    require(afterRetry.revision.number == 2 && afterRetry.documents.size() == 2 &&
                afterRetry.documents.back().chunks().size() == 2,
            "retry must publish exactly one complete document");
}

void invalidDatabase(const fs::path& directory) {
    requireError(SqliteKnowledgeStore::open(directory / "absent" / "library.sqlite"),
                 AppError::StoreUnavailable, "missing parent must produce an open error");

    const auto damaged = directory / "damaged.sqlite";
    const std::string invalidBytes = "This is not an SQLite file.";
    writeFixture(damaged, invalidBytes);
    requireError(SqliteKnowledgeStore::open(damaged), AppError::StoreCorrupt,
                 "invalid database bytes must produce a corruption error");
    require(readFixture(damaged) == invalidBytes, "opening a damaged database must preserve its bytes");

    const auto truncated = directory / "truncated.sqlite";
    writeFixture(truncated, "");
    requireError(SqliteKnowledgeStore::open(truncated), AppError::StoreCorrupt,
                 "an existing zero-byte database must be rejected");
    require(readFixture(truncated).empty(), "a truncated database must not be reinitialized");

    const auto unsupported = directory / "unsupported.sqlite";
    openStore(unsupported);
    executeFixtureSql(unsupported, "PRAGMA user_version = 99");
    const auto previous = readFixture(unsupported);
    requireError(SqliteKnowledgeStore::open(unsupported), AppError::StoreSchemaUnsupported,
                 "unknown schema version must be rejected");
    require(readFixture(unsupported) == previous, "opening an unknown version must preserve its bytes");

    const auto foreign = directory / "foreign.sqlite";
    executeFixtureSql(foreign, "CREATE TABLE other_data (value TEXT); INSERT INTO other_data VALUES ('keep');");
    const auto original = readFixture(foreign);
    requireError(SqliteKnowledgeStore::open(foreign), AppError::StoreSchemaUnsupported,
                 "an unrelated database must be rejected");
    require(readFixture(foreign) == original, "opening an unrelated database must preserve its bytes");
}

void policyRevisions(const fs::path& directory, const fs::path& demo) {
    const auto path = directory / "policies.sqlite";
    const auto observer = openStore(path);
    const auto initial = requireValue(observer->snapshot(), "reading initial library");
    const auto other = requireValue(openStore(directory / "other.sqlite")->snapshot(), "reading second library");
    require(initial.revision.number == 0 && initial.policy == DocumentPolicy::LocalOnly &&
                initial.revision.libraryId != other.revision.libraryId,
            "each new library needs its own identity and closed policy at revision zero");
    const TxtReader reader;
    auto writer = openStore(path);
    IndexingService index(reader, *writer);
    require(requireValue(writer->setLibraryPolicy(DocumentPolicy::AllowRemote), "granting library access"),
            "library grant must be a change");
    requireValue(index.import(demo / "contract.txt"), "importing into an open library");
    const auto mixed = requireValue(observer->snapshot(), "reading from another connection");
    require(mixed.revision.number == 2 && mixed.revision.libraryId == initial.revision.libraryId &&
                mixed.policy == DocumentPolicy::AllowRemote && mixed.documents.size() == 1 &&
                mixed.documents.front().policy() == DocumentPolicy::LocalOnly,
            "library access must not grant access to a new document");
    const auto id = mixed.documents.front().id();
    require(requireValue(writer->setDocumentPolicy(id, DocumentPolicy::AllowRemote), "granting document access"),
            "document grant must be a change");
    require(!requireValue(writer->setDocumentPolicy(id, DocumentPolicy::AllowRemote), "repeating document grant") &&
                !requireValue(writer->setLibraryPolicy(DocumentPolicy::AllowRemote), "repeating library grant"),
            "repeating a policy must return false");
    requireError(writer->setDocumentPolicy(id + 1, DocumentPolicy::LocalOnly), AppError::StoreWriteFailed,
                 "a missing document must report an error");

    executeFixtureSql(path, R"sql(
        CREATE TRIGGER fail_revision BEFORE UPDATE OF revision ON library BEGIN
            SELECT RAISE(ABORT, 'test revision failure');
        END;
    )sql");
    requireError(writer->setDocumentPolicy(id, DocumentPolicy::LocalOnly), AppError::StoreWriteFailed,
                 "a failed revision update must abort the document policy change");
    requireError(writer->setLibraryPolicy(DocumentPolicy::LocalOnly), AppError::StoreWriteFailed,
                 "a failed revision update must abort the library policy change");
    requireError(index.import(demo / "decisions.txt"), AppError::StoreWriteFailed,
                 "a failed revision update must abort document publication");

    const auto latest = requireValue(openStore(path)->snapshot(), "reading persisted policies and revision");
    require(latest.revision.number == 3 && latest.revision.libraryId == initial.revision.libraryId &&
                latest.policy == DocumentPolicy::AllowRemote && latest.documents.size() == 1 &&
                latest.documents.front().policy() == DocumentPolicy::AllowRemote,
            "only committed changes may survive reopening");
    require(mixed.documents.front().policy() == DocumentPolicy::LocalOnly && initial.documents.empty(),
            "snapshots already returned must retain the state of their own revisions");
}

void createLegacyLibrary(const fs::path& path) {
    executeFixtureSql(path, R"sql(
        CREATE TABLE documents (
            id INTEGER PRIMARY KEY, source TEXT NOT NULL,
            content TEXT NOT NULL COLLATE BINARY UNIQUE,
            policy INTEGER NOT NULL CHECK (policy IN (0, 1))
        ) STRICT;
        CREATE TABLE chunks (
            document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
            line INTEGER NOT NULL CHECK (line > 0), text TEXT NOT NULL,
            PRIMARY KEY (document_id, line)
        ) STRICT, WITHOUT ROWID;
        PRAGMA application_id = 0x52414742;
        PRAGMA user_version = 1;
        INSERT INTO documents VALUES
            (42, 'decisions.txt', char(10, 10, 10) || 'Резервное копирование выполняется каждую пятницу.', 1);
        INSERT INTO chunks VALUES (42, 4, 'Резервное копирование выполняется каждую пятницу.');
    )sql");
}

void migrate(const fs::path& directory) {
    const auto path = directory / "legacy.sqlite";
    createLegacyLibrary(path);
    const auto backup = directory / "legacy.sqlite.v1-backup" / "library.sqlite";
    const auto store = openStore(path);
    require(fixtureScalar(path, "PRAGMA user_version") == 7, "opened database must advance to v7");
    require(fixtureScalar(backup, "PRAGMA user_version") == 1 &&
                fixtureScalar(backup, "SELECT document_id FROM chunks WHERE line = 4") == 42,
            "backup must retain the v1 schema, document ID and line");

    const auto documents = requireValue(store->snapshot(), "reading migrated TXT").documents;
    require(documents.size() == 1 && documents.front().policy() == DocumentPolicy::AllowRemote &&
                documents.front().content() == std::string("\n\n\n") + decisionsQuote,
            "migration must preserve TXT content and policy");
    requireCitation(*store, decisionsQuestion, 42, "decisions.txt", decisionsQuote);
    requireCitation(*openStore(path), decisionsQuestion, 42, "decisions.txt", decisionsQuote);
    require(!fs::exists(directory / "legacy.sqlite.v1-backup-1"),
            "reopening v5 must not repeat the migration");
}

void migrationFailure(const fs::path& directory) {
    const auto path = directory / "legacy.sqlite";
    createLegacyLibrary(path);
    executeFixtureSql(path, "CREATE VIEW chunks_v1 AS SELECT * FROM chunks");
    requireError(SqliteKnowledgeStore::open(path), AppError::StoreMigrationFailed,
                 "a failed migration must report an error");
    require(fixtureScalar(path, "PRAGMA user_version") == 1 &&
                fixtureScalar(path, "SELECT id FROM documents") == 42 &&
                fixtureScalar(path, "SELECT line FROM chunks") == 4,
            "migration failure must roll back DDL and preserve the original data");
    const auto backup = directory / "legacy.sqlite.v1-backup" / "library.sqlite";
    require(fixtureScalar(backup, "PRAGMA user_version") == 1 &&
                fixtureScalar(backup, "SELECT id FROM documents") == 42,
            "a failed migration must leave the v1 backup available");
    executeFixtureSql(path, "DROP VIEW chunks_v1");
    requireCitation(*openStore(path), decisionsQuestion, 42, "decisions.txt", decisionsQuote);
    require(fixtureScalar(backup, "SELECT count(*) FROM sqlite_schema WHERE name = 'chunks_v1'") == 1,
            "retry must not overwrite the original v1 backup");
}

void migratePdfEra(const fs::path& directory) {
    const auto path = directory / "pdf-era.sqlite";
    executeFixtureSql(path, R"sql(
        CREATE TABLE documents (
            id INTEGER PRIMARY KEY, source TEXT NOT NULL, content TEXT NOT NULL COLLATE BINARY,
            policy INTEGER NOT NULL CHECK (policy IN (0, 1)), page_layout TEXT NOT NULL,
            UNIQUE (content, page_layout)
        ) STRICT;
        CREATE TABLE chunks (
            document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
            line INTEGER NOT NULL CHECK (line > 0), text TEXT NOT NULL, page INTEGER CHECK (page > 0),
            PRIMARY KEY (document_id, line)
        ) STRICT, WITHOUT ROWID;
        PRAGMA application_id = 0x52414742;
        PRAGMA user_version = 2;
        INSERT INTO documents VALUES
            (7, 'decisions.txt', 'Резервное копирование выполняется каждую пятницу.', 1, ''),
            (42, 'protocol.pdf', 'Orion snapshot runs every Friday.', 0, '0,33,');
        INSERT INTO chunks VALUES
            (7, 4, 'Резервное копирование выполняется каждую пятницу.', NULL),
            (42, 1, 'Orion snapshot runs every Friday.', 2);
    )sql");
    const auto store = openStore(path);
    require(fixtureScalar(path, "PRAGMA user_version") == 7, "v2 must advance to v7");
    const auto backup = directory / "pdf-era.sqlite.v2-backup" / "library.sqlite";
    require(fixtureScalar(backup, "PRAGMA user_version") == 2 &&
                fixtureScalar(backup, "SELECT page FROM chunks WHERE document_id = 42") == 2,
            "v2 backup must preserve the PDF page and document ID");
    requireCitation(*store, decisionsQuestion, 7, "decisions.txt", decisionsQuote);
    const KeywordRetriever retriever;
    MockProvider provider;
    RagService service(*store, retriever, provider);
    const auto pdf = requireValue(service.ask("Orion"), "reading migrated PDF");
    require(pdf.citation() && pdf.citation()->documentId() == 42 &&
                pdf.citation()->source() == "protocol.pdf" && pdf.citation()->page() == 2 &&
                !pdf.citation()->docx() && pdf.citation()->quote() == "Orion snapshot runs every Friday.",
            "migration must preserve PDF citation, page and ID");
}

void migrateDocxEra(const fs::path& directory) {
    const auto path = directory / "docx-era.sqlite";
    executeFixtureSql(path, R"sql(
        CREATE TABLE documents (
            id INTEGER PRIMARY KEY, source TEXT NOT NULL, content TEXT NOT NULL COLLATE BINARY,
            policy INTEGER NOT NULL CHECK (policy IN (0, 1)), position_layout TEXT NOT NULL,
            UNIQUE (content, position_layout)
        ) STRICT;
        CREATE TABLE chunks (
            document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
            line INTEGER NOT NULL, text TEXT NOT NULL, page INTEGER,
            docx_part TEXT, docx_paragraph INTEGER, docx_table INTEGER, docx_row INTEGER, docx_cell INTEGER,
            PRIMARY KEY (document_id, line)
        ) STRICT, WITHOUT ROWID;
        PRAGMA application_id = 0x52414742;
        PRAGMA user_version = 3;
        INSERT INTO documents VALUES (17, 'register.docx', 'Intro' || char(10) || 'Orion', 1, 'docx:layout');
        INSERT INTO chunks VALUES
            (17, 1, 'Intro', NULL, 'word/document.xml', 4, NULL, NULL, NULL),
            (17, 2, 'Orion', NULL, 'word/document.xml', NULL, 1, 2, 3);
    )sql");
    const auto store = openStore(path);
    const auto backup = directory / "docx-era.sqlite.v3-backup" / "library.sqlite";
    require(fixtureScalar(backup, "PRAGMA user_version") == 3 &&
                fixtureScalar(backup, "SELECT docx_row FROM chunks WHERE document_id = 17 AND line = 2") == 2 &&
                fixtureScalar(path, "PRAGMA user_version") == 7, "migration must preserve a readable v3 backup");
    const auto snapshot = requireValue(store->snapshot(), "reading migrated v3 library");
    require(snapshot.revision.number == 0 && snapshot.policy == DocumentPolicy::LocalOnly &&
                snapshot.documents.size() == 1, "migration starts a closed library at its baseline revision");
    const auto& doc = snapshot.documents.front();
    require(doc.id() == 17 && doc.content() == "Intro\nOrion" && doc.source() == "register.docx" &&
                doc.policy() == DocumentPolicy::AllowRemote && doc.positionLayout() == "docx:layout" &&
                doc.chunks().size() == 2 && doc.chunks()[0].line() == 1 && doc.chunks()[0].text() == "Intro" &&
                doc.chunks()[0].docx() == DocxPosition{"word/document.xml", std::size_t{4}} &&
                doc.chunks()[1].line() == 2 && doc.chunks()[1].text() == "Orion" &&
                doc.chunks()[1].docx() == DocxPosition{"word/document.xml", TableCell{1, 2, 3}},
            "migration must preserve the document identity, policy and both DOCX anchor types");
    require(requireValue(openStore(path)->snapshot(), "reopening migrated library").revision == snapshot.revision &&
                !fs::exists(directory / "docx-era.sqlite.v3-backup-1"),
            "reopening must preserve the migrated identity without repeating the migration");
}

void migrateRevisionEra(const fs::path& directory, const fs::path& demo, int version) {
    const auto path = directory / "revision-era.sqlite";
    IndexRevision previous;
    {
        const auto store = openStore(path);
        const TxtReader reader;
        IndexingService index(reader, *store);
        requireValue(index.import(demo / "decisions.txt"), "preparing a revisioned library");
        previous = requireValue(store->snapshot(), "reading the previous revision").revision;
        if (version == 5) {
            const KeywordRetriever keyword;
            MockProvider mock;
            ValidatingLlmProvider validated(mock);
            CachingLlmProvider cached(validated, *store);
            RagService oldService(*store, keyword, cached, store.get());
            require(!requireValue(oldService.ask(decisionsQuestion), "seeding v5 cache and history").fromCache(),
                    "old keyword profile must have its own completed answer");
        }
    }
    executeFixtureSql(path, "DROP TABLE source_blocks; ALTER TABLE chunks DROP COLUMN last_char;"
                            "ALTER TABLE chunks DROP COLUMN first_char; ALTER TABLE chunks DROP COLUMN source_line;"
                            "ALTER TABLE history DROP COLUMN last_char; ALTER TABLE history DROP COLUMN first_char;");
    if (version <= 5) {
        executeFixtureSql(path, "DROP TRIGGER index_chunk; DROP TABLE chunks_fts; DROP TABLE embeddings; DROP TABLE search_profile");
        executeFixtureSql(path, version == 4 ? "DROP TABLE answer_cache; DROP TABLE history; PRAGMA user_version = 4"
                                             : "PRAGMA user_version = 5");
    } else {
        executeFixtureSql(path, "UPDATE search_profile SET settings='unicode61-bm25-cos-rrf-v1/20/60/8/2'; PRAGMA user_version=6");
    }
    const auto migrated = openStore(path);
    const auto backup = directory / ("revision-era.sqlite.v" + std::to_string(version) + "-backup") / "library.sqlite";
    ++previous.number;  // Migration publishes a new index format.
    require(fixtureScalar(backup, "PRAGMA user_version") == version &&
                fixtureScalar(backup, "SELECT count(*) FROM documents") == 1 &&
                fixtureScalar(path, "PRAGMA user_version") == 7 &&
                fixtureScalar(path, "SELECT count(*) FROM chunks_fts WHERE chunks_fts MATCH 'копирование'") == 1 &&
                requireValue(migrated->snapshot(), "reading the migrated revision").revision == previous,
            "migration must back up the database, populate FTS and publish a new revision");
    const auto history = requireValue(migrated->history(), "checking migrated history");
    if (version == 5) {
        require(history.size() == 1 && history.front().outdated, "v5 history must survive as a previous revision");
        MockProvider mock;
        ValidatingLlmProvider validated(mock);
        CachingLlmProvider cached(validated, *migrated);
        RagService service(*migrated, *migrated, cached);
        require(!requireValue(service.ask(decisionsQuestion), "first BM25 answer").fromCache(),
                "schema migration must invalidate keyword-era answers");
    } else {
        require(history.empty(), "v4 must start with empty history");
    }
    requireCitation(*migrated, decisionsQuestion, 1, "decisions.txt", decisionsQuote);
}

class CountingProvider final : public ILlmProvider {
public:
    ModelProfile configured{ProcessingType::Local, "probe"};
    int calls = 0;
    bool invalidPosition = false;
    bool fail = false;
    std::string lastQuestion;

    ModelProfile profile() const override { return configured; }
    Result<ProposedCitation> generate(const GenerationRequest& request) override {
        ++calls;
        lastQuestion = request.question.text();
        if (fail) {
            return AppError::StoreUnavailable;
        }
        auto citation = ProposedCitation::fromChunk(request.evidence.front());
        if (invalidPosition) {
            citation.docx.reset();
            citation.page = 999;
        }
        return citation;
    }
};

void cacheAndHistory(const fs::path& directory) {
    const auto path = directory / "answers.sqlite";
    const KeywordRetriever retriever;
    const std::string original = " \tOrion\r\nsnapshot ";
    const DocxPosition location{"word/document.xml", TableCell{1, 2, 3}};
    {
        const auto store = openStore(path);
        executeFixtureSql(path, "UPDATE library SET library_id = '11111111111111111111111111111111'");
        requireValue(store->save("source.docx", "Orion", {{3, "Orion", {}, location}},
                                 DocumentPolicy::LocalOnly), "saving source");
        CountingProvider probe;
        ValidatingLlmProvider validated(probe);
        CachingLlmProvider cached(validated, *store);
        RagService service(*store, retriever, cached, store.get());
        const auto first = requireValue(service.ask(original), "asking the first question");
        require(!first.fromCache() && first.citation() && probe.calls == 1 &&
                    probe.lastQuestion == "Orion\nsnapshot", "miss must generate once with canonical newlines");
        require(requireValue(service.ask("Orion\rsnapshot"), "asking equivalent question").fromCache() &&
                    probe.calls == 1, "repeated question must skip generation");
        require(fixtureScalar(path, "SELECT count(*) FROM answer_cache WHERE hex(key) = "
                     "'AC40638C0F3A33755FF6AC1B634674BE26B4F40533905D66451D6B0D6866E0CB'") == 1,
                "persisted digest must match the independent SHA-256 test vector");
    }
    const auto store = openStore(path);
    CountingProvider probe;
    ValidatingLlmProvider validated(probe);
    CachingLlmProvider cached(validated, *store);
    RagService service(*store, retriever, cached, store.get());
    require(requireValue(service.ask("Orion\nsnapshot"), "asking after reopen").fromCache() && probe.calls == 0,
            "a persisted hit must skip a freshly constructed provider");
    const auto history = requireValue(store->history(), "reading durable questions");
    require(history.size() == 3 && history[0].question == original && !history[0].answer.fromCache() &&
                history[1].answer.fromCache() && history[2].answer.fromCache(), "both cache hits must be in history");
    for (const auto& entry : history) {
        const auto& c = entry.answer.citation();
        require(!entry.timeUtc.empty() && entry.timeUtc.back() == 'Z' && !entry.outdated &&
                    entry.revision == IndexRevision{std::string(32, '1'), 1} && entry.profile == probe.configured &&
                    c && c->documentId() == 1 && c->line() == 3 && c->source() == "source.docx" &&
                    c->quote() == "Orion" && !c->page() && c->docx() == location,
                "history must preserve the revision, profile, UTC time and complete citation");
    }
    for (const auto question : {"orion\nsnapshot", "Orion  snapshot", "Orion snapshot", "Orion snapshot!"}) {
        const int before = probe.calls;
        require(!requireValue(service.ask(question), "checking distinct question").fromCache() &&
                    probe.calls == before + 1 && probe.lastQuestion == question,
                "case, punctuation and inner whitespace must distinguish standalone questions");
    }
    requireValue(store->setLibraryPolicy(DocumentPolicy::AllowRemote), "changing revision");
    const int beforeRevision = probe.calls;
    require(!requireValue(service.ask(original), "asking after revision change").fromCache() &&
                probe.calls == beforeRevision + 1 && requireValue(store->history(), "checking stale history")[0].outdated,
            "a changed revision must miss and mark previous entries outdated");
    const auto baseline = probe.configured;
    for (auto field : {&ModelProfile::modelId, &ModelProfile::generationParameters, &ModelProfile::responseFormat,
                       &ModelProfile::endpoint, &ModelProfile::credentialScope}) {
        probe.configured = baseline;
        probe.configured.*field += "-changed";
        const int before = probe.calls;
        require(!requireValue(service.ask(original), "asking after profile change").fromCache() &&
                    probe.calls == before + 1, "changing any identity field must cause a miss");
    }
    const auto noMatch = requireValue(service.ask("absent"), "asking without evidence");
    require(!noMatch.citation() && !requireValue(store->history(), "reading no-match history").back().answer.citation(),
            "insufficient evidence must be a completed history result");
    requireValue(store->clearHistory(), "clearing history");
    require(requireValue(store->history(), "reading cleared history").empty() &&
                requireValue(service.ask(original), "checking cache after clearing history").fromCache(),
            "history cleanup must preserve the cached answer");
}

class CountingCache final : public IAnswerCacheStore {
public:
    explicit CountingCache(IAnswerCacheStore& store) : store_(store) {}
    mutable int reads = 0;
    int writes = 0;
    Result<std::optional<ChunkReference>> lookup(const GenerationRequest& request) const override {
        ++reads;
        return store_.lookup(request);
    }
    Result<bool> remember(const GenerationRequest& request, ChunkReference citation) override {
        ++writes;
        return store_.remember(request, citation);
    }
private:
    IAnswerCacheStore& store_;
};

void cacheAccessAndFailures(const fs::path& directory) {
    const auto store = openStore(directory / "private.sqlite");
    const KeywordRetriever retriever;
    CountingProvider probe;
    CountingCache cache(*store);
    ValidatingLlmProvider validated(probe);
    CachingLlmProvider cached(validated, cache);
    RagService service(*store, retriever, cached, store.get());
    requireValue(store->save("private.pdf", "Orion", {{1, "Orion", 2}}, DocumentPolicy::LocalOnly), "saving PDF");
    probe.configured.processing = ProcessingType::Remote;
    requireError(service.ask("Orion"), AppError::RemoteProcessingDenied, "private library must block external profile");
    requireValue(store->setLibraryPolicy(DocumentPolicy::AllowRemote), "granting only library access");
    requireError(service.ask("Orion"), AppError::RemoteProcessingDenied, "private document must block external profile");
    require(cache.reads == 0 && cache.writes == 0 && probe.calls == 0, "denial must precede even reading the cache");

    probe.configured.processing = ProcessingType::Local;
    probe.invalidPosition = true;
    for (int i = 0; i < 2; ++i) {
        requireError(service.ask("Orion"), AppError::InvalidCitation, "invalid page must fail on every attempt");
    }
    probe.invalidPosition = false;
    probe.fail = true;
    requireError(service.ask("Orion"), AppError::StoreUnavailable, "provider failure must propagate");
    require(cache.reads == 3 && probe.calls == 3 && cache.writes == 0 &&
                requireValue(store->history(), "checking rejected requests").empty(),
            "failures must leave both cache and completed history empty");
    probe.fail = false;
    require(!requireValue(service.ask("Orion"), "retrying corrected citation").fromCache() &&
                requireValue(service.ask("Orion"), "reading corrected citation").fromCache() &&
                probe.calls == 4 && cache.writes == 1, "only a validated retry may populate the cache");
}

class FixtureEmbeddings final : public IEmbeddingProvider {
public:
    std::string id = "fixture-v1";
    bool fail = false;
    std::string profileId() const override { return id; }
    std::size_t dimension() const override { return 2; }
    Result<std::size_t> documentTokenCount(std::string_view text) const override { return text.size(); }
    Result<std::vector<float>> embed(std::string_view text, EmbeddingInput input) const override {
        if (input == EmbeddingInput::Document && text.size() > 400) return AppError::EmbeddingFailed;
        if (text.ends_with("TailMarker")) return std::vector<float>{1, 0};
        if (fail && text == "Archive") return AppError::EmbeddingFailed;
        if (text == "Archive" || text == "restore point") return std::vector<float>{3, 0};
        if (text == "needle" || text.ends_with(" C")) return std::vector<float>{1, 0};
        if (text.ends_with(" B")) return std::vector<float>{9, 1};
        if (text.ends_with(" D")) return std::vector<float>{6, 4};
        return std::vector<float>{0, 5};
    }
};

void longBlocks(const fs::path& directory, const IEmbeddingProvider& model) {
    const auto path = directory / "long.sqlite";
    const auto file = directory / "long.txt";
    std::string line = "🙂 ";
    for (int i = 0; i < 2300; ++i) line += "данные ";
    line += "TailMarker";
    require(requireValue(model.documentTokenCount(line), "measuring input") > 2048,
            "the regression fixture must exceed the original runtime limit");
    writeFixture(file, "intro\r\n\r\n" + line + "\r\nafter");
    const TxtReader reader;
    auto store = openStore(path);
    requireValue(store->configureSearch(&model), "starting hybrid search");
    const auto report = requireValue(IndexingService(reader, *store).import(file), "importing the long TXT");
    const auto snapshot = requireValue(store->snapshot(), "reading long TXT");
    const auto& pieces = snapshot.documents.at(0).chunks();
    require(report.chunks == pieces.size() && pieces.size() > 3 && pieces.front().line() == 1 &&
            pieces.back().line() == 4 && !pieces.back().characters(), "neighbouring source lines must stay intact");
    std::size_t covered = 0;
    for (std::size_t i = 1; i + 1 < pieces.size(); ++i) {
        const auto& piece = pieces[i];
        const auto range = piece.characters();
        require(piece.line() == 3 && piece.chunkId() > pieces[i - 1].chunkId() && range &&
                range->first <= covered + 1 && range->last > covered, "source ranges must cover the line in order");
        require(requireValue(model.documentTokenCount(piece.text()), "counting slice tokens") <= 400,
                "slice exceeds token budget");
        if (covered) {
            require(range->first <= covered, "expected overlap inside this long block");
            const auto overlapChars = covered - range->first + 1;
            const auto overlap = decodeUtf8(piece.text()).value().substr(0, overlapChars); // BMP in this fixture's overlap
            require(requireValue(model.documentTokenCount(encodeUtf8(overlap).value()), "counting overlap") <= 60,
                    "overlap exceeds token budget");
        }
        covered = range->last;
    }
    // SQLite length/substr count Unicode scalar values independently of the C++ splitter.
    require(fixtureScalar(path, "SELECT length(text) FROM source_blocks WHERE line=3") == covered &&
            fixtureScalar(path, "SELECT count(*) FROM chunks c JOIN source_blocks b ON b.document_id=c.document_id "
                                "AND b.line=c.source_line WHERE c.first_char IS NOT NULL AND "
                                "c.text <> substr(b.text,c.first_char,c.last_char-c.first_char+1)") == 0,
            "every quoted range must select exactly the original UTF-8 substring");
    require(fixtureScalar(path, "SELECT count(*) FROM embeddings") == pieces.size() &&
            fixtureScalar(path, "SELECT count(*) FROM chunks_fts") == pieces.size(), "every slice must be searchable");
    auto askTail = [&] {
        MockProvider mock;
        ValidatingLlmProvider valid(mock);
        CachingLlmProvider cached(valid, *store);
        return requireValue(RagService(*store, *store, cached, store.get()).ask("TailMarker"), "asking for the tail");
    };
    const auto first = askTail();
    require(first.citation() && first.citation()->line() == 3 && first.citation()->quote().ends_with("TailMarker") &&
            first.citation()->characters() == pieces[pieces.size() - 2].characters(), "wrong tail citation");
    store.reset();
    store = openStore(path);
    requireValue(store->configureSearch(&model), "reopening the same profile");
    const auto second = askTail();
    const auto history = requireValue(store->history(), "reading history after restart");
    require(second.fromCache() && second.citation() && second.citation()->characters() == first.citation()->characters() &&
            history.at(0).answer.citation() && history.at(0).answer.citation()->characters() == first.citation()->characters() &&
            history.at(0).answer.citation()->quote() == first.citation()->quote(), "tail citation changed after reopening");
    requireValue(store->configureSearch(nullptr, {}, true), "reindexing with FTS alone");
    require(fixtureScalar(path, "SELECT count(*) FROM source_blocks") == 3 &&
            fixtureScalar(path, "SELECT max(last_char) = (SELECT length(text) FROM source_blocks WHERE line=3) "
                                "FROM chunks WHERE source_line=3") == 1, "reindex must start from intact source blocks");
}

void splitPositions(const fs::path& directory) {
    auto store = openStore(directory / "positions.sqlite");
    const DocxPosition cell{"word/document.xml", TableCell{2, 3, 4}};
    const std::string block(900, 'x');
    requireValue(store->save("source.pdf", block, {{7, block, 2}}, DocumentPolicy::LocalOnly), "splitting PDF");
    requireValue(store->save("source.docx", block, {{9, block, {}, cell}}, DocumentPolicy::LocalOnly, "cell"),
                 "splitting DOCX");
    store.reset();
    store = openStore(directory / "positions.sqlite");
    const auto snapshot = requireValue(store->snapshot(), "reopening split positions");
    for (const auto& doc : snapshot.documents) {
        require(doc.chunks().size() == 3, "unexpected FTS-only slice count");
        for (const auto& chunk : doc.chunks()) {
            require(chunk.characters() && (doc.id() == 1 ? chunk.page() == 2 && chunk.line() == 7 :
                    chunk.docx() == cell && chunk.line() == 9), "lost the source page or DOCX anchor");
        }
    }
}

void labelledContext(const fs::path& directory, const fs::path& demo, const IEmbeddingProvider& model) {
    auto store = openStore(directory / "context.sqlite");
    requireValue(store->configureSearch(&model), "activating Qwen");
    const TxtReader txt;
    const PdfReader pdf;
    const DocxReader docx;
    IndexingService index(txt, *store);
    requireValue(index.import(demo / "contract.txt"), "importing contract");
    requireValue(index.import(demo / "decisions.txt"), "importing decisions");
    struct Label { const char* question; DocumentId document; std::size_t line; };
    const Label labels[] = {
        {"Резервное копирование", 2, 4}, {"В какой день делают страховочную копию данных проекта?", 2, 4},
        {"Каков срок приёмки результата?", 1, 4}, {"Что решили о переносе обучения?", 1, 5}
    };
    for (const int documents : {2, 4}) {
        if (documents == 4) {
            requireValue(IndexingService(pdf, *store).import(demo / "backup-protocol.pdf"), "importing PDF");
            requireValue(IndexingService(docx, *store).import(demo / "backup-register.docx"), "importing DOCX");
        }
        const auto snapshot = requireValue(store->snapshot(), "reading labelled corpus");
        for (const auto& label : labels) {
            const auto query = requireValue(Question::fromText(label.question), "creating labelled query");
            const auto context = requireValue(store->retrieve(query, snapshot), "retrieving labelled evidence");
            const auto found = std::ranges::find_if(context, [&](const Chunk& c) {
                return c.documentId() == label.document && c.line() == label.line;
            });
            std::cout << documents << " documents, target " << label.document << ':' << label.line << " rank ";
            if (found == context.end()) std::cout << "missing";
            else std::cout << (1 + (found - context.begin()));
            std::cout << '/' << context.size() << '\n';
            require(found != context.end(), "the labelled source was absent from the retrieval context");
        }
    }
}

void reciprocalRankFusion(const fs::path& directory) {
    auto store = openStore(directory / "fusion.sqlite");
    // Equal document lengths; lexical order A,B,D,C by frequency, cosine order C,B,D,A.
    for (const std::string text : {"needle needle needle needle A", "needle needle needle filler B",
                                   "needle needle filler filler D", "needle filler filler filler C"}) {
        requireValue(store->save("term.txt", text, {{1, text}}, DocumentPolicy::LocalOnly), "saving ranking fixture");
    }
    const auto question = requireValue(Question::fromText("needle"), "ranking question");
    const auto lexical = requireValue(store->retrieve(question, requireValue(store->snapshot(), "FTS snapshot")), "BM25 search");
    require(lexical.size() == 4 && lexical.front().documentId() == 1, "highest term frequency must win BM25");
    FixtureEmbeddings provider;
    requireValue(store->configureSearch(&provider, {}, true), "building hybrid ranking fixture");
    const auto fused = requireValue(store->retrieve(question, requireValue(store->snapshot(), "hybrid snapshot")), "RRF search");
    require(fused.size() == 4 && fused[0].documentId() == 2 && fused[1].documentId() == 1 &&
            fused[2].documentId() == 4 && fused[3].documentId() == 3,
            "second place on both lists must win RRF; symmetric ties must use the chunk key");
}

void hybridSearch(const fs::path& directory) {
    const auto path = directory / "hybrid.sqlite";
    FixtureEmbeddings model;
    const DocxPosition cell{"word/document.xml", TableCell{3, 2, 1}};
    {
        auto store = openStore(path);
        requireValue(store->configureSearch(&model), "activating fixture profile");
        requireValue(store->save("one.txt", "Alpha", {{4, "Alpha"}}, DocumentPolicy::LocalOnly), "saving TXT");
        requireValue(store->save("two.pdf", "Протокол", {{5, "Протокол", 2}}, DocumentPolicy::LocalOnly), "saving PDF");
        requireValue(store->save("three.docx", "Archive", {{7, "Archive", {}, cell}}, DocumentPolicy::LocalOnly), "saving DOCX");
    }
    auto store = openStore(path);
    requireValue(store->configureSearch(&model), "reopening fixture index");
    const auto snapshot = requireValue(store->snapshot(), "reading hybrid snapshot");
    const auto search = [&](std::string q) {
        return requireValue(store->retrieve(requireValue(Question::fromText(std::move(q)), "question"), snapshot), "retrieving");
    };
    const auto exact = search("ПРОТОКОЛ \" OR (NEAR * -");
    require(!exact.empty() && exact.front().page() == 2 && exact.front().documentId() == 2 && exact.front().line() == 5,
            "escaped FTS syntax and Unicode term must rank the PDF first over a semantic tie");
    const auto paraphrase = search("restore point");
    require(!paraphrase.empty() && paraphrase.front().docx() == cell && paraphrase.front().documentId() == 3 &&
            paraphrase.front().line() == 7, "vector-only paraphrase must recover the persisted DOCX cell");
    const auto tied = search("unknown");
    require(tied.size() == 3 && tied[0].documentId() == 1 && tied[0].line() == 4 && tied[1].documentId() == 2,
            "equal cosine scores must sort by the persistent chunk key");
    CountingProvider probe;
    ValidatingLlmProvider validated(probe);
    CachingLlmProvider cached(validated, *store);
    RagService rag(*store, *store, cached);
    require(!requireValue(rag.ask("restore point"), "first question").fromCache() &&
            requireValue(rag.ask("restore point"), "repeated question").fromCache() && probe.calls == 1, "expected cache hit");
    FixtureEmbeddings next;
    next.id = "fixture-v2";
    requireError(store->configureSearch(&next), AppError::SearchProfileMismatch, "profile switch needs reindex");
    next.fail = true;
    requireError(store->configureSearch(&next, {}, true), AppError::EmbeddingFailed, "partial reindex must roll back");
    require(requireValue(store->snapshot(), "after failed rebuild").revision == snapshot.revision &&
            requireValue(rag.ask("restore point"), "old index after failed rebuild").fromCache(), "previous index must stay usable");
    model.fail = true;
    requireError(store->save("broken.txt", "combined", {{1,"First"}, {2,"Archive"}}, DocumentPolicy::LocalOnly),
                 AppError::EmbeddingFailed, "second embedding must abort publication");
    require(fixtureScalar(path, "SELECT count(*) FROM embeddings") == 3 &&
            fixtureScalar(path, "SELECT count(*) FROM documents") == 3 &&
            fixtureScalar(path, "SELECT count(*) FROM chunks") == 3 &&
            fixtureScalar(path, "SELECT count(*) FROM chunks_fts") == 3 &&
            requireValue(store->snapshot(), "after failed import").revision == snapshot.revision,
            "failed import must preserve all index components");
    next.fail = false;
    requireValue(store->configureSearch(&next, {}, true), "rebuilding for second model");
    require(fixtureScalar(path, "SELECT count(*) FROM embeddings WHERE model='fixture-v2' AND dimension=2 AND length(vector)=8") == 3 &&
            !requireValue(rag.ask("restore point"), "cache across model change").fromCache(), "all vectors and cache must switch");
    SearchSettings tuning;
    tuning.rrfConstant = 40;
    requireValue(store->configureSearch(&next, tuning, true), "changing retrieval settings");
    require(!requireValue(rag.ask("restore point"), "cache across settings change").fromCache() && probe.calls == 3,
            "a change in ranking settings must invalidate the old answer");
    requireError(store->retrieve(requireValue(Question::fromText("Archive"), "question"), snapshot),
                 AppError::SearchProfileMismatch, "a stale admitted snapshot must not use the new index");
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4 && argc != 5) {
        return 2;
    }
    try {
        const TemporaryDirectory directory(argv[3]);
        const std::wstring_view test = argv[1];
        if (test == L"reopen") {
            reopen(directory.path(), argv[2]);
        } else if (test == L"duplicate") {
            duplicate(directory.path(), argv[2]);
        } else if (test == L"rollback") {
            rollback(directory.path(), argv[2]);
        } else if (test == L"invalid_database") {
            invalidDatabase(directory.path());
        } else if (test == L"migration") {
            migrate(directory.path());
        } else if (test == L"migration_failure") {
            migrationFailure(directory.path());
        } else if (test == L"migration_v2") {
            migratePdfEra(directory.path());
        } else if (test == L"migration_v3") {
            migrateDocxEra(directory.path());
        } else if (test == L"migration_v4") {
            migrateRevisionEra(directory.path(), argv[2], 4);
        } else if (test == L"migration_v5") {
            migrateRevisionEra(directory.path(), argv[2], 5);
        } else if (test == L"migration_v6") {
            migrateRevisionEra(directory.path(), argv[2], 6);
        } else if (test == L"long_blocks") {
            longBlocks(directory.path(), FixtureEmbeddings{});
        } else if (test == L"split_positions") {
            splitPositions(directory.path());
        } else if (test == L"real_chunks" && argc == 5) {
            const auto model = requireValue(openLocalEmbeddings(argv[4]), "loading local GGUF");
            longBlocks(directory.path(), *model);
            labelledContext(directory.path(), argv[2], *model);
        } else if (test == L"hybrid_search") {
            hybridSearch(directory.path());
        } else if (test == L"rrf") {
            reciprocalRankFusion(directory.path());
        } else if (test == L"policy_revisions") {
            policyRevisions(directory.path(), argv[2]);
        } else if (test == L"cache_history") {
            cacheAndHistory(directory.path());
        } else if (test == L"cache_access_errors") {
            cacheAccessAndFailures(directory.path());
        } else {
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
