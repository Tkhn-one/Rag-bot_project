#include "rag/application/services.hpp"
#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/local_embeddings.hpp"
#include "rag/infrastructure/remote_api_provider.hpp"
#include "rag/infrastructure/sqlite_knowledge_store.hpp"
#include "rag/infrastructure/utf8.hpp"
#include "rag/presentation/cli.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

class DocumentReader final : public rag::IDocumentReader {
public:
    rag::Result<rag::ExtractedDocument> read(const std::filesystem::path& path) const override {
        const rag::IDocumentReader* readers[] = {&txt_, &pdf_, &docx_};
        for (const auto* reader : readers) {
            auto result = reader->read(path);
            const auto* error = std::get_if<rag::AppError>(&result);
            if (!error || *error != rag::AppError::UnsupportedFormat) {
                return result;
            }
        }
        return rag::AppError::UnsupportedFormat;
    }

private:
    rag::TxtReader txt_;
    rag::PdfReader pdf_;
    rag::DocxReader docx_;
};

void print(DWORD handleId, std::ostream& stream, const std::string& utf8) {
    if (utf8.empty()) {
        return;
    }
    const auto handle = GetStdHandle(handleId);
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        if (const auto wide = rag::decodeUtf8(utf8)) {
            DWORD written = 0;
            if (WriteConsoleW(handle, wide->data(), static_cast<DWORD>(wide->size()),
                              &written, nullptr) && written == wide->size()) {
                return;
            }
        }
    }
    stream << utf8;
}

int usage() {
    print(STD_ERROR_HANDLE, std::cerr,
          "Использование: rag_cli <файл.txt|pdf|docx> [<ещё-файл> ...] \"<вопрос>\"\n"
          "               rag_cli --db <база> [--embedding-model <GGUF>] [<файл> ...] \"<вопрос>\"\n"
          "               rag_cli --db <база> [--embedding-model <GGUF>] --openai\n"
          "                       [--model <ID>] [--dev-env-key] [--credential-scope <метка>] \"<вопрос>\"\n"
          "               rag_cli --documents <база>\n"
          "               rag_cli --policy <база> library|document <ID для document> allow-remote|local-only\n"
          "               rag_cli --history <база>\n"
          "               rag_cli --clear-history <база>\n"
          "               rag_cli --reindex <база> [--embedding-model <GGUF>]\n");
    return 2;
}

std::optional<rag::DocumentId> parseDocumentId(std::wstring_view value) {
    const auto text = rag::encodeUtf8(value);
    if (!text || text->empty()) return std::nullopt;
    rag::DocumentId id = 0;
    const auto end = text->data() + text->size();
    const auto parsed = std::from_chars(text->data(), end, id);
    if (parsed.ec != std::errc{} || parsed.ptr != end || id == 0) return std::nullopt;
    return id;
}

std::optional<std::string> parseProfileLabel(std::wstring_view value) {
    constexpr std::wstring_view allowed =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.:";
    if (value.empty() || value.size() > 128 || value.starts_with(L"--") ||
        value.find_first_not_of(allowed) != std::wstring_view::npos) {
        return std::nullopt;
    }
    return rag::encodeUtf8(value);
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    const auto command = argc > 1 ? std::wstring_view(argv[1]) : std::wstring_view{};
    const bool viewHistory = command == L"--history";
    const bool clearHistory = command == L"--clear-history";
    const bool historyCommand = viewHistory || clearHistory;
    const bool documents = command == L"--documents";
    const bool setPolicy = command == L"--policy";
    const bool policyCommand = documents || setPolicy;
    const bool reindex = command == L"--reindex";
    const bool useDatabase = command == L"--db" || reindex || policyCommand || historyCommand;
    if ((useDatabase && argc < 3) || ((historyCommand || documents) && argc != 3)) return usage();

    std::optional<rag::DocumentId> document;
    std::optional<rag::DocumentPolicy> policy;
    if (setPolicy) {
        if (argc != 5 && argc != 6) return usage();
        const std::wstring_view scope(argv[3]);
        if (scope == L"document" && argc == 6) {
            document = parseDocumentId(argv[4]);
            if (!document) return usage();
        } else if (scope != L"library" || argc != 5) {
            return usage();
        }
        const std::wstring_view value(argv[argc - 1]);
        if (value == L"allow-remote") {
            policy = rag::DocumentPolicy::AllowRemote;
        } else if (value == L"local-only") {
            policy = rag::DocumentPolicy::LocalOnly;
        } else {
            return usage();
        }
    }

    int firstFile = useDatabase ? 3 : 1;
    const wchar_t* embeddingModel = nullptr;
    if (!historyCommand && !policyCommand && argc > firstFile &&
        std::wstring_view(argv[firstFile]) == L"--embedding-model") {
        if (argc <= firstFile + 1) return usage();
        embeddingModel = argv[firstFile + 1];
        firstFile += 2;
    }

    const bool openAi = command == L"--db" && argc > firstFile &&
                        std::wstring_view(argv[firstFile]) == L"--openai";
    auto keySource = rag::ApiKeySource::CredentialManager;
    std::optional<std::string> modelId;
    std::optional<std::string> credentialScope;
    if (openAi) {
        ++firstFile;
        while (firstFile < argc - 1) {
            const std::wstring_view flag(argv[firstFile++]);
            if (flag == L"--dev-env-key" && keySource == rag::ApiKeySource::CredentialManager) {
                keySource = rag::ApiKeySource::DevelopmentEnvironment;
            } else if ((flag == L"--model" || flag == L"--credential-scope") && firstFile < argc - 1) {
                auto& option = flag == L"--model" ? modelId : credentialScope;
                if (option) return usage();
                option = parseProfileLabel(argv[firstFile++]);
                if (!option) return usage();
            } else {
                return usage();
            }
        }
    }
    if ((reindex && argc != firstFile) ||
        (!historyCommand && !policyCommand && !reindex &&
         (argc <= firstFile || (!useDatabase && argc <= firstFile + 1) ||
          (openAi && std::wstring_view(argv[argc - 1]).starts_with(L"--"))))) {
        return usage();
    }

    auto opened = useDatabase ? rag::SqliteKnowledgeStore::open(argv[2], command == L"--db" && !openAi)
                              : rag::SqliteKnowledgeStore::openInMemory();
    if (const auto* error = std::get_if<rag::AppError>(&opened)) {
        print(STD_ERROR_HANDLE, std::cerr, std::string(rag::describeError(*error)) + '\n');
        return 1;
    }
    auto store = std::get<std::unique_ptr<rag::SqliteKnowledgeStore>>(std::move(opened));
    std::ostringstream output;
    std::ostringstream errors;
    if (historyCommand || policyCommand) {
        const int status = historyCommand ? rag::runHistoryCli(*store, clearHistory, output, errors)
                                          : rag::runPolicyCli(*store, document, policy, output, errors);
        print(STD_OUTPUT_HANDLE, std::cout, output.str());
        print(STD_ERROR_HANDLE, std::cerr, errors.str());
        return status;
    }

    std::unique_ptr<rag::IEmbeddingProvider> embeddings;
    if (embeddingModel) {
        auto model = rag::openLocalEmbeddings(embeddingModel);
        if (const auto* error = std::get_if<rag::AppError>(&model)) {
            print(STD_ERROR_HANDLE, std::cerr, std::string(rag::describeError(*error)) + '\n');
            return 1;
        }
        embeddings = std::get<std::unique_ptr<rag::IEmbeddingProvider>>(std::move(model));
    }
    auto configured = store->configureSearch(embeddings.get(), {}, reindex);
    if (const auto* error = std::get_if<rag::AppError>(&configured)) {
        print(STD_ERROR_HANDLE, std::cerr, std::string(rag::describeError(*error)) + '\n');
        return 1;
    }
    if (reindex) {
        print(STD_OUTPUT_HANDLE, std::cout, "Поисковый индекс перестроен.\n");
        return 0;
    }
    const auto question = rag::encodeUtf8(argv[argc - 1]);
    if (!question) {
        print(STD_ERROR_HANDLE, std::cerr, "Не удалось прочитать вопрос.\n");
        return 2;
    }

    const DocumentReader reader;
    rag::MockProvider mock;
    rag::WinHttpTransport transport;
    const rag::WindowsOpenAiSecrets secrets(keySource);
    const std::string scopePrefix = keySource == rag::ApiKeySource::CredentialManager ? "windows:" : "env:";
    rag::RemoteApiProvider remote(transport, secrets,
                                  modelId.value_or(std::string(rag::RemoteApiProvider::defaultModel)),
                                  scopePrefix + credentialScope.value_or("default"));
    rag::ILlmProvider& provider = openAi ? static_cast<rag::ILlmProvider&>(remote)
                                       : static_cast<rag::ILlmProvider&>(mock);
    rag::ValidatingLlmProvider validated(provider);
    rag::CachingLlmProvider cached(validated, *store);
    rag::IndexingService index(reader, *store);
    rag::RagService service(*store, *store, cached, store.get());

    const std::vector<std::filesystem::path> files(argv + firstFile, argv + argc - 1);
    const auto status = rag::runCli(index, service, files, *question, provider.profile().processing, output, errors);
    print(STD_OUTPUT_HANDLE, std::cout, output.str());
    print(STD_ERROR_HANDLE, std::cerr, errors.str());
    return status;
}
