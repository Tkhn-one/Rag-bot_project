#include "rag/application/services.hpp"
#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/utf8.hpp"

#include <fstream>
#include <iostream>
#include <string_view>

namespace {

using namespace rag;

class ProbeProvider final : public ILlmProvider {
public:
    explicit ProbeProvider(ProposedCitation proposal, ProcessingType processing = ProcessingType::Local)
        : proposal_(std::move(proposal)), processing_(processing) {}

    ModelProfile profile() const override {
        return {processing_, processing_ == ProcessingType::Local ? "test-local" : "test-remote"};
    }

    Result<ProposedCitation> generate(const Question&, std::span<const Chunk>) override {
        ++calls_;
        return proposal_;
    }

    int calls() const noexcept { return calls_; }

private:
    ProposedCitation proposal_;
    ProcessingType processing_;
    int calls_ = 0;
};

class FailingKnowledgeStore final : public IKnowledgeStore {
public:
    Result<std::size_t> save(std::string, std::string, std::vector<DocumentLine>,
                             DocumentPolicy, std::string) override {
        return AppError::StoreWriteFailed;
    }

    Result<LibrarySnapshot> snapshot() const override { return AppError::StoreReadFailed; }
    Result<bool> setLibraryPolicy(DocumentPolicy) override { return AppError::StoreWriteFailed; }
    Result<bool> setDocumentPolicy(DocumentId, DocumentPolicy) override { return AppError::StoreWriteFailed; }
};

class PagedReader final : public IDocumentReader {
public:
    Result<ExtractedDocument> read(const std::filesystem::path&) const override {
        return ExtractedDocument{{{"", 1}, {"Orion snapshot.", 2}}};
    }
};

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

template <typename T>
bool isError(const Result<T>& result, AppError expected) {
    const auto* error = std::get_if<AppError>(&result);
    return error && *error == expected;
}

bool writeFixture(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return static_cast<bool>(file);
}

bool encodings(const std::filesystem::path& buildDir) {
    const TxtReader reader;
    const auto path = buildDir / "encodings.txt";
    const char littleEndian[] = {'\xFF', '\xFE', 'A', '\0'};
    const char bigEndian[] = {'\xFE', '\xFF', '\0', 'A'};
    for (const std::string_view input : {
             std::string_view{"\xEF\xBB\xBF" "A"},
             std::string_view{littleEndian, sizeof(littleEndian)},
             std::string_view{bigEndian, sizeof(bigEndian)}}) {
        if (!expect(writeFixture(path, input), "could not write encoding fixture")) {
            return false;
        }
        const auto content = reader.read(path);
        const auto* document = std::get_if<ExtractedDocument>(&content);
        if (!expect(document && document->sections.size() == 1 &&
                        document->sections.front().text == "A" && !document->sections.front().page,
                    "BOM and encoding must be handled without assigning a page to TXT")) {
            return false;
        }
    }

    return expect(writeFixture(path, "\xC3\x28"), "could not write invalid TXT") &&
           expect(isError(reader.read(path), AppError::InvalidEncoding), "malformed UTF-8 accepted");
}

constexpr std::size_t demoLine = 4;
constexpr std::string_view contractQuote =
    "Срок приёмки результата составляет десять рабочих дней.";
constexpr std::string_view decisionsQuote =
    "Резервное копирование выполняется каждую пятницу.";

bool cites(const Result<Answer>& result, DocumentId id, std::string_view source,
           std::string_view quote) {
    const auto* answer = std::get_if<Answer>(&result);
    return answer && answer->citation() && answer->citation()->documentId() == id &&
           answer->citation()->source() == source && answer->citation()->line() == demoLine &&
           !answer->citation()->page() &&
           answer->citation()->quote() == quote;
}

std::vector<IndexedDocument> storedDocuments(const InMemoryKnowledgeStore& store) {
    return std::get<LibrarySnapshot>(store.snapshot()).documents;
}

class CountingRetriever final : public IRetriever {
public:
    std::vector<Chunk> retrieve(const Question& question,
                                std::span<const IndexedDocument> documents) const override {
        ++calls;
        return KeywordRetriever{}.retrieve(question, documents);
    }

    mutable int calls = 0;
};

bool providerAccess(const std::filesystem::path& demo) {
    InMemoryKnowledgeStore store;
    const TxtReader reader;
    IndexingService index(reader, store);
    if (!expect(std::holds_alternative<ImportReport>(index.import(demo)) &&
                    std::holds_alternative<ImportReport>(index.import(demo.parent_path() / "decisions.txt")),
                "both documents must import")) {
        return false;
    }
    const auto documents = storedDocuments(store);
    if (!expect(documents.size() == 2, "the selected library must contain both documents")) {
        return false;
    }
    const auto contract = documents.front().id();
    const auto decisions = documents.back().id();
    CountingRetriever retriever;
    ProbeProvider remote({demoLine, std::string(contractQuote), contract}, ProcessingType::Remote);
    RagService external(store, retriever, remote);
    if (!expect(store.setLibraryPolicy(DocumentPolicy::AllowRemote) == Result<bool>(true) &&
                    store.setDocumentPolicy(contract, DocumentPolicy::AllowRemote) == Result<bool>(true),
                "explicit grants must leave the decisions document closed") ||
        !expect(isError(external.ask("срок приёмки"), AppError::RemoteProcessingDenied) &&
                    retriever.calls == 0 && remote.calls() == 0,
                "a closed document must block even a question about the open document before retrieval")) {
        return false;
    }

    ProbeProvider local({demoLine, std::string(decisionsQuote), decisions});
    RagService offline(store, retriever, local);
    if (!expect(cites(offline.ask("Резервное копирование"), decisions, "decisions.txt", decisionsQuote) &&
                    retriever.calls == 1 && local.calls() == 1 && remote.calls() == 0,
                "the same mixed library must answer locally from the closed document") ||
        !expect(store.setDocumentPolicy(decisions, DocumentPolicy::AllowRemote) == Result<bool>(true) &&
                    store.setLibraryPolicy(DocumentPolicy::LocalOnly) == Result<bool>(true),
                "both documents should now allow remote processing while the library forbids it") ||
        !expect(isError(external.ask("срок приёмки"), AppError::RemoteProcessingDenied) &&
                    retriever.calls == 1 && remote.calls() == 0,
                "a closed library must block before retrieval even when all documents allow remote access")) {
        return false;
    }
    return expect(store.setLibraryPolicy(DocumentPolicy::AllowRemote) == Result<bool>(true) &&
                      cites(external.ask("срок приёмки"), contract, "contract.txt", contractQuote) &&
                      retriever.calls == 2 && remote.calls() == 1,
                  "the external test provider must run once when all permissions are granted");
}

bool duplicateImport(IndexingService& index, InMemoryKnowledgeStore& store,
                     const std::filesystem::path& demo, const std::filesystem::path& buildDir) {
    const auto copy = buildDir / "duplicate_import.txt";
    const auto initial = storedDocuments(store);
    const auto& first = initial.front();
    const auto id = first.id();
    const auto chunks = first.chunks().size();
    if (!expect(writeFixture(copy, "\xEF\xBB\xBF" + first.content()),
                "could not write a copy of the original text")) {
        return false;
    }

    const auto importedOriginal = index.import(demo);
    const auto importedCopy = index.import(copy);
    const auto* originalReport = std::get_if<ImportReport>(&importedOriginal);
    const auto* copyReport = std::get_if<ImportReport>(&importedCopy);
    if (!expect(originalReport && originalReport->chunks == chunks &&
                    copyReport && copyReport->chunks == chunks &&
                    store.save("remote.txt", first.content(), {{1, "Replacement"}},
                               DocumentPolicy::AllowRemote) == Result<std::size_t>(chunks),
                "reimporting known content must return the original chunk count")) {
        return false;
    }

    const auto documents = storedDocuments(store);
    return expect(documents.size() == 1 && documents.front().id() == id &&
                      documents.front().source() == "contract.txt" &&
                      documents.front().policy() == DocumentPolicy::LocalOnly,
                  "duplicates must preserve the original document identity, name and policy");
}

bool pagedCitation() {
    const PagedReader reader;
    InMemoryKnowledgeStore store;
    IndexingService index(reader, store);
    const KeywordRetriever retriever;
    MockProvider provider;
    RagService service(store, retriever, provider);
    const auto imported = index.import("pages.pdf");
    const auto* report = std::get_if<ImportReport>(&imported);
    if (!expect(report && report->chunks == 1 && report->pagesWithoutText == std::vector<std::size_t>{1},
                "a page without text must be listed while preserving the next page's text")) {
        return false;
    }
    const auto result = service.ask("Orion");
    const auto* answer = std::get_if<Answer>(&result);
    if (!expect(answer && answer->citation() && answer->citation()->page() == 2,
                "the citation must point to page 2")) {
        return false;
    }
    const auto& citation = *answer->citation();
    for (auto page : {std::optional<std::size_t>{1}, std::optional<std::size_t>{}}) {
        ProbeProvider invalid({citation.line(), citation.quote(), citation.documentId(), page});
        RagService invalidService(store, retriever, invalid);
        if (!expect(isError(invalidService.ask("Orion"), AppError::InvalidCitation),
                    "a missing or incorrect page must invalidate a PDF citation")) {
            return false;
        }
    }
    return true;
}

bool run(std::string_view test, const std::filesystem::path& demo,
         const std::filesystem::path& buildDir) {
    if (test == "encodings") {
        return encodings(buildDir);
    }
    if (test == "paged_citation") {
        return pagedCitation();
    }
    if (test == "provider_access") {
        return providerAccess(demo);
    }

    const TxtReader reader;
    const KeywordRetriever retriever;
    ProbeProvider probe({4, "Выдуманная цитата."});
    if (test == "store_errors") {
        FailingKnowledgeStore store;
        IndexingService index(reader, store);
        RagService service(store, retriever, probe);
        return expect(isError(index.import(demo), AppError::StoreWriteFailed),
                      "the indexing service must propagate store write failures") &&
               expect(isError(service.ask("срок приёмки"), AppError::StoreReadFailed) &&
                          probe.calls() == 0,
                      "the RAG service must propagate store read failures without generation");
    }

    InMemoryKnowledgeStore store;
    IndexingService index(reader, store);
    RagService withProbe(store, retriever, probe);

    if (test == "empty_file") {
        const auto path = buildDir / "empty_document.txt";
        if (!expect(writeFixture(path, " \t\r\n\r\n"), "could not write blank TXT")) {
            return false;
        }
        const auto imported = index.import(path);
        const auto result = withProbe.ask("срок");
        const auto* answer = std::get_if<Answer>(&result);
        return expect(isError(imported, AppError::EmptyDocument), "expected EmptyDocument") &&
               expect(storedDocuments(store).empty(), "failed import must not publish a document") &&
               expect(answer && !answer->citation() && probe.calls() == 0,
                      "asking against an empty store must skip generation");
    }
    if (test == "missing_file") {
        const auto path = buildDir / "absent-fixture-directory" / "missing.txt";
        return expect(isError(index.import(path), AppError::FileUnavailable),
                      "missing file must return FileUnavailable");
    }

    if (!expect(std::holds_alternative<ImportReport>(index.import(demo)),
                "the demo contract must import successfully")) {
        return false;
    }

    if (test == "duplicate_import") {
        return duplicateImport(index, store, demo, buildDir);
    }
    if (test == "second_document" || test == "wrong_document_citation" ||
        test == "failed_import_preserves_library") {
        const auto firstId = storedDocuments(store).front().id();
        if (test == "failed_import_preserves_library") {
            const auto path = buildDir / "failed_import.txt";
            if (!expect(writeFixture(path, "Valid first line.\r\n\xC3\x28"),
                        "could not write malformed TXT") ||
                !expect(isError(index.import(path), AppError::InvalidEncoding),
                        "malformed TXT must return InvalidEncoding")) {
                return false;
            }
            const auto documents = storedDocuments(store);
            if (!expect(documents.size() == 1 && documents.front().id() == firstId,
                        "failed import must leave the original library intact")) {
                return false;
            }
        }

        const auto second = demo.parent_path() / "decisions.txt";
        if (!expect(std::holds_alternative<ImportReport>(index.import(second)),
                    "the second document must import successfully")) {
            return false;
        }
        const auto documents = storedDocuments(store);
        if (!expect(documents.size() == 2,
                    "the library must contain both successful imports")) {
            return false;
        }
        const auto& secondDocument = documents.back();
        const auto secondId = secondDocument.id();
        if (test == "wrong_document_citation") {
            for (const auto& proposal : {
                     ProposedCitation{demoLine, std::string(decisionsQuote), firstId},
                     ProposedCitation{999, std::string(decisionsQuote), secondId},
                     ProposedCitation{demoLine, "Выдуманная цитата.", secondId}}) {
                ProbeProvider invalid(proposal);
                RagService service(store, retriever, invalid);
                if (!expect(isError(service.ask("Резервное копирование"), AppError::InvalidCitation) &&
                                invalid.calls() == 1,
                            "validation must check document ID, line and quote against evidence")) {
                    return false;
                }
            }
            return true;
        }

        MockProvider mock;
        RagService service(store, retriever, mock);
        return expect(firstId != secondId && secondDocument.policy() == DocumentPolicy::LocalOnly,
                      "the second document needs its own ID and the default policy") &&
               expect(cites(service.ask("срок приёмки"), firstId, "contract.txt", contractQuote),
                      "the original line 4 must remain searchable") &&
               expect(cites(service.ask("Резервное копирование"), secondId,
                            "decisions.txt", decisionsQuote),
                      "the second document must supply its own line 4");
    }

    if (test == "answer") {
        MockProvider mock;
        RagService service(store, retriever, mock);
        const auto result = service.ask("Какой СРОК приёмки?");
        const auto* answer = std::get_if<Answer>(&result);
        return expect(answer && answer->citation() &&
                          answer->citation()->source() == "contract.txt" &&
                          answer->citation()->line() == 4 &&
                          !answer->citation()->page() &&
                          answer->citation()->quote() ==
                              "Срок приёмки результата составляет десять рабочих дней.",
                      "expected the original quotation from line 4");
    }
    if (test == "empty_question") {
        return expect(isError(withProbe.ask(" \t\n "), AppError::EmptyQuestion),
                      "expected EmptyQuestion") &&
               expect(probe.calls() == 0, "blank question must skip generation");
    }
    if (test == "no_match") {
        const auto result = withProbe.ask("Астрономия");
        const auto* answer = std::get_if<Answer>(&result);
        return expect(answer && !answer->citation(), "expected insufficient evidence") &&
               expect(probe.calls() == 0, "no evidence must skip generation");
    }
    if (test == "unknown_citation") {
        const auto documentId = storedDocuments(store).front().id();
        ProbeProvider unknown({999, std::string(contractQuote), documentId});
        RagService service(store, retriever, unknown);
        return expect(isError(service.ask("срок приёмки"), AppError::InvalidCitation) &&
                          unknown.calls() == 1,
                      "a citation with an unknown line must be rejected");
    }
    if (test == "altered_quote") {
        const auto documentId = storedDocuments(store).front().id();
        ProbeProvider altered({demoLine, "Выдуманная цитата.", documentId});
        RagService service(store, retriever, altered);
        return expect(isError(service.ask("срок приёмки"), AppError::InvalidCitation) &&
                          altered.calls() == 1,
                      "an altered quotation must be rejected");
    }
    return expect(false, "unknown test");
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
        return 2;
    }
    const auto name = rag::encodeUtf8(argv[1]);
    return name && run(*name, argv[2], argv[3]) ? 0 : 1;
}
