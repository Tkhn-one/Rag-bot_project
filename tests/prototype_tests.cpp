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
    explicit ProbeProvider(ProposedCitation proposal) : proposal_(std::move(proposal)) {}

    Result<ProposedCitation> generate(const Question&, std::span<const Chunk>) override {
        ++calls_;
        return proposal_;
    }

    int calls() const noexcept { return calls_; }

private:
    ProposedCitation proposal_;
    int calls_ = 0;
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
        const auto* text = std::get_if<std::string>(&content);
        if (!expect(text && *text == "A", "BOM and encoding must be handled")) {
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
           answer->citation()->quote() == quote;
}

bool duplicateImport(IndexingService& index, const InMemoryKnowledgeStore& store,
                     const std::filesystem::path& demo, const std::filesystem::path& buildDir) {
    const auto copy = buildDir / "duplicate_import.txt";
    const auto& first = store.documents().front();
    const auto id = first.id();
    const auto chunks = first.chunks().size();
    if (!expect(writeFixture(copy, "\xEF\xBB\xBF" + first.content()),
                "could not write a copy of the original text")) {
        return false;
    }

    return expect(index.import(demo) == Result<std::size_t>(chunks) &&
                      index.import(copy) == Result<std::size_t>(chunks),
                  "reimporting known content must succeed") &&
           expect(store.documents().size() == 1 && store.documents().front().id() == id &&
                      store.documents().front().source() == "contract.txt" &&
                      store.documents().front().policy() == DocumentPolicy::LocalOnly,
                  "duplicates must preserve the original document identity, name and policy");
}

bool run(std::string_view test, const std::filesystem::path& demo,
         const std::filesystem::path& buildDir) {
    if (test == "encodings") {
        return encodings(buildDir);
    }

    const TxtReader reader;
    InMemoryKnowledgeStore store;
    const KeywordRetriever retriever;
    IndexingService index(reader, store);
    ProbeProvider probe({4, "Выдуманная цитата."});
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
               expect(!store.current(), "failed import must not publish a document") &&
               expect(answer && !answer->citation() && probe.calls() == 0,
                      "asking against an empty store must skip generation");
    }
    if (test == "missing_file") {
        const auto path = buildDir / "absent-fixture-directory" / "missing.txt";
        return expect(isError(index.import(path), AppError::FileUnavailable),
                      "missing file must return FileUnavailable");
    }

    if (!expect(std::holds_alternative<std::size_t>(index.import(demo)),
                "the demo contract must import successfully")) {
        return false;
    }

    if (test == "duplicate_import") {
        return duplicateImport(index, store, demo, buildDir);
    }
    if (test == "second_document" || test == "wrong_document_citation" ||
        test == "failed_import_preserves_library") {
        const auto firstId = store.documents().front().id();
        if (test == "failed_import_preserves_library") {
            const auto path = buildDir / "failed_import.txt";
            if (!expect(writeFixture(path, "Valid first line.\r\n\xC3\x28"),
                        "could not write malformed TXT") ||
                !expect(isError(index.import(path), AppError::InvalidEncoding) &&
                            store.documents().size() == 1 && store.documents().front().id() == firstId,
                        "failed import must leave the original library intact")) {
                return false;
            }
        }

        const auto second = demo.parent_path() / "decisions.txt";
        if (!expect(std::holds_alternative<std::size_t>(index.import(second)) &&
                        store.documents().size() == 2,
                    "the library must contain both successful imports")) {
            return false;
        }
        const auto& secondDocument = store.documents().back();
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
        ProbeProvider unknown({999, "Срок приёмки результата составляет десять рабочих дней."});
        RagService service(store, retriever, unknown);
        return expect(isError(service.ask("срок приёмки"), AppError::InvalidCitation) &&
                          unknown.calls() == 1,
                      "a citation with an unknown line must be rejected");
    }
    if (test == "altered_quote") {
        return expect(isError(withProbe.ask("срок приёмки"), AppError::InvalidCitation) &&
                          probe.calls() == 1,
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
