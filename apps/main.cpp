#include "rag/application/services.hpp"
#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/utf8.hpp"
#include "rag/presentation/cli.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <iostream>
#include <sstream>

namespace {

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
    if (argc != 3) {
        print(STD_ERROR_HANDLE, std::cerr,
              "Использование: rag_cli <файл.txt> \"<вопрос>\"\n");
        return 2;
    }

    const auto question = rag::encodeUtf8(argv[2]);
    if (!question) {
        print(STD_ERROR_HANDLE, std::cerr, "Не удалось прочитать вопрос.\n");
        return 2;
    }

    const rag::TxtReader reader;
    rag::InMemoryKnowledgeStore store;
    const rag::KeywordRetriever retriever;
    rag::MockProvider provider;
    rag::IndexingService index(reader, store);
    rag::RagService service(store, retriever, provider);

    std::ostringstream output;
    std::ostringstream errors;
    const auto status = rag::runCli(index, service, argv[1], *question, output, errors);
    print(STD_OUTPUT_HANDLE, std::cout, output.str());
    print(STD_ERROR_HANDLE, std::cerr, errors.str());
    return status;
}
