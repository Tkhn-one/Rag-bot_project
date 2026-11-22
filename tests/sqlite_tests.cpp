#include "rag/application/services.hpp"
#include "rag/infrastructure/adapters.hpp"
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
    require(fixtureScalar(path, "PRAGMA user_version") == 4, "opened database must advance to v4");
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
            "reopening v4 must not repeat the migration");
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
    require(fixtureScalar(path, "PRAGMA user_version") == 4, "v2 must advance to v4");
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
                fixtureScalar(path, "PRAGMA user_version") == 4, "migration must preserve a readable v3 backup");
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

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
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
        } else if (test == L"policy_revisions") {
            policyRevisions(directory.path(), argv[2]);
        } else {
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
