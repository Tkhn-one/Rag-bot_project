#include "rag/application/services.hpp"
#include "rag/infrastructure/adapters.hpp"
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

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    const auto command = argc > 1 ? std::wstring_view(argv[1]) : std::wstring_view{};
    const bool viewHistory = command == L"--history";
    const bool clearHistory = command == L"--clear-history";
    const bool historyCommand = viewHistory || clearHistory;
    const bool useDatabase = command == L"--db";
    const int firstFile = useDatabase ? 3 : 1;
    const int minimumArguments = useDatabase ? 4 : 3;
    if ((historyCommand && argc != 3) || (!historyCommand && argc < minimumArguments)) {
        print(STD_ERROR_HANDLE, std::cerr,
              "Использование: rag_cli <файл.txt|pdf|docx> [<ещё-файл> ...] \"<вопрос>\"\n"
              "               rag_cli --db <путь-к-базе> [<файл.txt|pdf|docx> ...] \"<вопрос>\"\n"
              "               rag_cli --history <путь-к-базе>\n"
              "               rag_cli --clear-history <путь-к-базе>\n");
        return 2;
    }

    auto opened = (useDatabase || historyCommand) ? rag::SqliteKnowledgeStore::open(argv[2], !historyCommand)
                                                  : rag::SqliteKnowledgeStore::openInMemory();
    if (const auto* error = std::get_if<rag::AppError>(&opened)) {
        print(STD_ERROR_HANDLE, std::cerr, std::string(rag::describeError(*error)) + '\n');
        return 1;
    }
    auto store = std::get<std::unique_ptr<rag::SqliteKnowledgeStore>>(std::move(opened));
    std::ostringstream output;
    std::ostringstream errors;
    if (historyCommand) {
        const int status = rag::runHistoryCli(*store, clearHistory, output, errors);
        print(STD_OUTPUT_HANDLE, std::cout, output.str());
        print(STD_ERROR_HANDLE, std::cerr, errors.str());
        return status;
    }

    const auto question = rag::encodeUtf8(argv[argc - 1]);
    if (!question) {
        print(STD_ERROR_HANDLE, std::cerr, "Не удалось прочитать вопрос.\n");
        return 2;
    }

    const DocumentReader reader;
    const rag::KeywordRetriever retriever;
    rag::MockProvider provider;
    rag::ValidatingLlmProvider validated(provider);
    rag::CachingLlmProvider cached(validated, *store);
    rag::IndexingService index(reader, *store);
    rag::RagService service(*store, retriever, cached, store.get());

    const std::vector<std::filesystem::path> files(argv + firstFile, argv + argc - 1);
    const auto status = rag::runCli(index, service, files, *question, output, errors);
    print(STD_OUTPUT_HANDLE, std::cout, output.str());
    print(STD_ERROR_HANDLE, std::cerr, errors.str());
    return status;
}
