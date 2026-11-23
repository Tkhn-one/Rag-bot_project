#include "rag/presentation/cli.hpp"
#include "rag/application/services.hpp"

#include <ostream>

namespace rag {

std::string_view describeError(AppError error) {
    switch (error) {
    case AppError::UnsupportedFormat:
        return "Выберите файл с расширением .txt, .pdf или .docx.";
    case AppError::FileUnavailable:
        return "Не удалось открыть или прочитать файл.";
    case AppError::FileTooLarge:
        return "Файл превышает допустимый размер (TXT — 8 МиБ, PDF и DOCX — 64 МиБ).";
    case AppError::InvalidEncoding:
        return "Не удалось прочитать кодировку. Сохраните файл в UTF-8.";
    case AppError::EmptyDocument:
        return "В TXT-файле нет текста.";
    case AppError::EmptyQuestion:
        return "Введите вопрос.";
    case AppError::InvalidCitation:
        return "Тестовый провайдер вернул неподтверждённую цитату.";
    case AppError::RemoteProcessingDenied:
        return "Внешняя обработка запрещена. Для неё требуется разрешение библиотеки и каждого документа.";
    case AppError::EmbeddingFailed:
        return "Не удалось загрузить модель или вычислить локальный вектор. Проверьте сборку, GGUF и длину фрагмента.";
    case AppError::SearchProfileMismatch:
        return "Индекс изменился или использует другой профиль. Повторите вопрос с тем же GGUF либо выполните --reindex.";
    case AppError::StoreReadFailed:
        return "Не удалось прочитать библиотеку документов.";
    case AppError::StoreWriteFailed:
        return "Не удалось сохранить изменения в библиотеке.";
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
    case AppError::DocxCorrupt:
        return "DOCX повреждён или содержит некорректную структуру ZIP или XML.";
    case AppError::DocxNoText:
        return "В поддерживаемых частях DOCX нет текста для поиска.";
    case AppError::DocxLimitExceeded:
        return "DOCX превышает ограничение распаковки или структуры XML.";
    }
    return "Не удалось выполнить запрос.";
}

namespace {

void printAnswer(const Answer& answer, std::ostream& output) {
    if (answer.fromCache()) {
        output << "Ответ из кэша.\n";
    }
    const auto& citation = answer.citation();
    if (!citation) {
        output << "В библиотеке не найдено подтверждения по словам вопроса.\n";
        return;
    }

    output << "Найденный фрагмент (тестовый провайдер)\n"
           << "«" << citation->quote() << "»\n"
           << "Источник: " << citation->source();
    if (const auto& docx = citation->docx()) {
        output << ", часть " << docx->part;
        if (const auto* paragraph = std::get_if<std::size_t>(&docx->anchor)) {
            output << ", абзац " << *paragraph;
        } else {
            const auto& cell = std::get<TableCell>(docx->anchor);
            output << ", таблица " << cell.table << ", строка " << cell.row << ", ячейка " << cell.cell;
        }
    } else if (const auto page = citation->page()) {
        output << ", страница " << *page;
    } else {
        output << ", строка " << citation->line();
    }
    output << " (ID документа " << citation->documentId() << ")\n";
}

}  // namespace

int runHistoryCli(IHistoryStore& store, bool clear, std::ostream& output, std::ostream& errors) {
    if (clear) {
        const auto result = store.clearHistory();
        if (const auto* error = std::get_if<AppError>(&result)) {
            errors << describeError(*error) << '\n';
            return 1;
        }
        output << "История очищена.\n";
        return 0;
    }
    const auto result = store.history();
    if (const auto* error = std::get_if<AppError>(&result)) {
        errors << describeError(*error) << '\n';
        return 1;
    }
    const auto& entries = std::get<std::vector<HistoryEntry>>(result);
    if (entries.empty()) {
        output << "История пуста.\n";
    } else {
        output << "Каждый вопрос задаётся отдельно.\n";
    }
    for (const auto& entry : entries) {
        output << '\n' << entry.timeUtc << " | " << entry.profile.modelId << " | ревизия "
               << entry.revision.number << (entry.outdated ? " (устарела)" : "") << '\n'
               << entry.question << '\n';
        printAnswer(entry.answer, output);
    }
    return 0;
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
            continue;
        }
        const auto& report = std::get<ImportReport>(imported);
        if (!report.pagesWithoutText.empty()) {
            const auto path = file.u8string();
            errors << std::string(path.begin(), path.end()) << ": страницы без текста — ";
            const char* separator = "";
            for (const auto page : report.pagesWithoutText) {
                errors << separator << page;
                separator = ", ";
            }
            errors << ". Остальной текст проиндексирован.\n";
        }
        if (report.incomplete) {
            const auto path = file.u8string();
            errors << std::string(path.begin(), path.end())
                   << ": DOCX извлечён частично. Неподдерживаемые элементы пропущены.\n";
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

    printAnswer(std::get<Answer>(result), output);
    return status;
}

}  // namespace rag
