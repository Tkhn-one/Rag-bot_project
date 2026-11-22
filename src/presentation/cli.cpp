#include "rag/presentation/cli.hpp"
#include "rag/application/services.hpp"

#include <ostream>

namespace rag {

std::string_view describeError(AppError error) {
    switch (error) {
    case AppError::UnsupportedFormat:
        return "Выберите файл с расширением .txt или .pdf.";
    case AppError::FileUnavailable:
        return "Не удалось открыть или прочитать файл.";
    case AppError::FileTooLarge:
        return "Файл превышает допустимый размер (TXT — 8 МиБ, PDF — 64 МиБ).";
    case AppError::InvalidEncoding:
        return "Не удалось прочитать кодировку. Сохраните файл в UTF-8.";
    case AppError::EmptyDocument:
        return "В TXT-файле нет текста.";
    case AppError::EmptyQuestion:
        return "Введите вопрос.";
    case AppError::InvalidCitation:
        return "Тестовый провайдер вернул неподтверждённую цитату.";
    case AppError::StoreReadFailed:
        return "Не удалось прочитать библиотеку документов.";
    case AppError::StoreWriteFailed:
        return "Не удалось сохранить документ в библиотеке.";
    case AppError::StoreUnavailable:
        return "Не удалось открыть базу SQLite. Проверьте путь и права доступа.";
    case AppError::StoreCorrupt:
        return "Файл базы SQLite повреждён или имеет неверный формат.";
    case AppError::StoreSchemaUnsupported:
        return "Файл использует неподдерживаемую схему библиотеки.";
    case AppError::StoreMigrationFailed:
        return "Не удалось сохранить резервную копию или обновить схему библиотеки.";
    case AppError::PdfCorrupt:
        return "PDF повреждён или не удалось извлечь текст.";
    case AppError::PdfEncrypted:
        return "PDF зашифрован. Выберите незашифрованную копию.";
    case AppError::PdfNoText:
        return "В PDF нет текстового слоя, доступного для поиска.";
    case AppError::PdfTextTooLarge:
        return "Объём извлечённого текста PDF превышает 32 МиБ.";
    }
    return "Не удалось выполнить запрос.";
}

int runCli(IndexingService& index, RagService& rag, std::span<const std::filesystem::path> files,
           std::string question, std::ostream& output, std::ostream& errors) {
    std::size_t failedImports = 0;
    for (const auto& file : files) {
        const auto imported = index.import(file);
        if (const auto* error = std::get_if<AppError>(&imported)) {
            const auto path = file.u8string();
            errors << std::string(path.begin(), path.end()) << ": " << describeError(*error) << '\n';
            ++failedImports;
        } else if (const auto& pages = std::get<ImportReport>(imported).pagesWithoutText; !pages.empty()) {
            const auto path = file.u8string();
            errors << std::string(path.begin(), path.end()) << ": страницы без текста — ";
            const char* separator = "";
            for (const auto page : pages) {
                errors << separator << page;
                separator = ", ";
            }
            errors << ". Остальной текст проиндексирован.\n";
        }
    }
    if (!files.empty() && failedImports == files.size()) {
        return 1;
    }

    const int status = failedImports == 0 ? 0 : 1;
    const auto result = rag.ask(std::move(question));
    if (const auto* error = std::get_if<AppError>(&result)) {
        errors << describeError(*error) << '\n';
        return 1;
    }

    const auto& citation = std::get<Answer>(result).citation();
    if (!citation) {
        output << "В библиотеке не найдено подтверждения по словам вопроса.\n";
        return status;
    }

    output << "Найденный фрагмент (тестовый провайдер)\n"
           << "«" << citation->quote() << "»\n"
           << "Источник: " << citation->source();
    if (const auto page = citation->page()) {
        output << ", страница " << *page;
    } else {
        output << ", строка " << citation->line();
    }
    output << " (ID документа " << citation->documentId() << ")\n";
    return status;
}

}  // namespace rag
