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
    rag::Result<std::vector<rag::TextSection>> read(const std::filesystem::path& path) const override {
        auto result = txt_.read(path);
        if (const auto* error = std::get_if<rag::AppError>(&result);
            error && *error == rag::AppError::UnsupportedFormat) {
            return pdf_.read(path);
        }
        return result;
    }

private:
    rag::TxtReader txt_;
    rag::PdfReader pdf_;
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
    const bool useDatabase = argc > 1 && std::wstring_view(argv[1]) == L"--db";
    const int firstFile = useDatabase ? 3 : 1;
    const int minimumArguments = useDatabase ? 4 : 3;
    if (argc < minimumArguments) {
        print(STD_ERROR_HANDLE, std::cerr,
              "Использование: rag_cli <файл.txt|pdf> [<ещё-файл> ...] \"<вопрос>\"\n"
              "               rag_cli --db <путь-к-базе> [<файл.txt|pdf> ...] \"<вопрос>\"\n");
        return 2;
    }

    const auto question = rag::encodeUtf8(argv[argc - 1]);
    if (!question) {
        print(STD_ERROR_HANDLE, std::cerr, "Не удалось прочитать вопрос.\n");
        return 2;
    }

    std::unique_ptr<rag::IKnowledgeStore> store;
    if (useDatabase) {
        auto opened = rag::SqliteKnowledgeStore::open(argv[2]);
        if (const auto* error = std::get_if<rag::AppError>(&opened)) {
            print(STD_ERROR_HANDLE, std::cerr, std::string(rag::describeError(*error)) + '\n');
            return 1;
        }
        store = std::get<std::unique_ptr<rag::SqliteKnowledgeStore>>(std::move(opened));
    } else {
        store = std::make_unique<rag::InMemoryKnowledgeStore>();
    }

    const DocumentReader reader;
    const rag::KeywordRetriever retriever;
    rag::MockProvider provider;
    rag::IndexingService index(reader, *store);
    rag::RagService service(*store, retriever, provider);

    const std::vector<std::filesystem::path> files(argv + firstFile, argv + argc - 1);
    std::ostringstream output;
    std::ostringstream errors;
    const auto status = rag::runCli(index, service, files, *question, output, errors);
    print(STD_OUTPUT_HANDLE, std::cout, output.str());
    print(STD_ERROR_HANDLE, std::cerr, errors.str());
    return status;
}
