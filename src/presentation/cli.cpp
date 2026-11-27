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
        return "Провайдер вернул неподтверждённую цитату.";
    case AppError::InvalidGeneration:
        return "Провайдер вернул ответ в неожиданном формате.";
    case AppError::RemoteContextTooLarge:
        return "Запрос превышает допустимый размер для OpenAI. Сократите вопрос.";
    case AppError::ApiKeyMissing:
        return "Ключ OpenAI отсутствует в выбранном источнике.";
    case AppError::ApiKeyUnavailable:
        return "Не удалось прочитать ключ OpenAI или ключ имеет неверный формат.";
    case AppError::RemoteUnauthorized:
        return "OpenAI отклонил ключ (HTTP 401).";
    case AppError::RemoteForbidden:
        return "OpenAI запретил доступ (HTTP 403). Проверьте доступ проекта к API и модели.";
    case AppError::RemoteRateLimited:
        return "OpenAI ограничил запросы или исчерпана квота (HTTP 429).";
    case AppError::RemoteServiceUnavailable:
        return "OpenAI временно недоступен (HTTP 5xx).";
    case AppError::RemoteHttpError:
        return "OpenAI отклонил запрос. Проверьте выбранную модель.";
    case AppError::RemoteNetworkError:
        return "Не удалось установить защищённое соединение с OpenAI или прочитать ответ.";
    case AppError::RemoteTimeout:
        return "Истекло время ожидания ответа OpenAI.";
    case AppError::RemoteRefusal:
        return "Модель OpenAI отказалась отвечать на этот вопрос.";
    case AppError::RemoteIncomplete:
        return "OpenAI вернул незавершённый ответ.";
    case AppError::RemoteProcessingDenied:
        return "Внешняя обработка запрещена. Для неё требуется разрешение библиотеки и каждого документа.";
    case AppError::Cancelled:
        return "Операция отменена.";
    case AppError::LibraryBusy:
        return "Библиотека занята. Повторите команду после завершения текущей операции.";
    case AppError::EmbeddingFailed:
        return "Не удалось загрузить модель или вычислить локальный вектор. Проверьте сборку, GGUF и длину фрагмента.";
    case AppError::LocalGenerationDisabled:
        return "Локальная генерация отключена. Используйте сборку WITH_LOCAL_LLM=ON.";
    case AppError::LocalModelUnavailable:
        return "GGUF генератора недоступен. Укажите обычный файл на локальном диске.";
    case AppError::LocalModelMismatch:
        return "Размер или SHA-256 модели отличается от официального Qwen3-4B-Q4_K_M.gguf.";
    case AppError::LocalModelLoadFailed:
        return "Не удалось загрузить генератор Qwen3-4B или создать контекст. Проверьте доступную память.";
    case AppError::LocalContextTooLarge:
        return "Вопрос и первый фрагмент превышают контекст локальной модели. Сократите вопрос.";
    case AppError::LocalGenerationFailed:
        return "Локальная модель не завершила ответ. Проверьте доступную память и лимит вывода.";
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

void printCitation(const Citation& citation, std::ostream& output) {
    output << "«" << citation.quote() << "»\n"
           << "Источник: " << citation.source();
    if (const auto& docx = citation.docx()) {
        output << ", часть " << docx->part;
        if (const auto* paragraph = std::get_if<std::size_t>(&docx->anchor)) {
            output << ", абзац " << *paragraph;
        } else {
            const auto& cell = std::get<TableCell>(docx->anchor);
            output << ", таблица " << cell.table << ", строка " << cell.row << ", ячейка " << cell.cell;
        }
    } else if (const auto page = citation.page()) {
        output << ", страница " << *page;
    } else {
        output << ", строка " << citation.line();
    }
    if (const auto range = citation.characters()) {
        if (citation.page()) output << ", строка " << citation.line();
        output << ", символы " << range->first << "–" << range->last;
    }
    output << " (ID документа " << citation.documentId() << ")\n";
}

void printAnswer(const Answer& answer, const ModelProfile& profile, std::ostream& output) {
    const bool extractive = profile.responseFormat == "extractive-v1";
    if (answer.fromCache()) output << "Ответ из кэша.\n";
    if (answer.status() == AnswerStatus::InsufficientEvidence) {
        output << (extractive
                   ? "В библиотеке не найдено подтверждения по словам вопроса.\n"
                   : "В библиотеке недостаточно подтверждений для ответа.\n");
        return;
    }
    if (extractive) output << "Найденный фрагмент (тестовый провайдер)\n";
    for (const auto& claim : answer.claims()) {
        if (!extractive) output << claim.text << '\n';
        printCitation(claim.citation, output);
    }
}

std::string_view policyName(DocumentPolicy policy) {
    return policy == DocumentPolicy::AllowRemote ? "AllowRemote" : "LocalOnly";
}

}  // namespace

int runSourceCli(IHistoryStore& history, ICitationSource& sources, std::size_t entry, std::size_t claim,
                 std::ostream& output, std::ostream& errors) {
    const auto result = history.history();
    if (const auto* error = std::get_if<AppError>(&result)) {
        errors << describeError(*error) << '\n';
        return 1;
    }
    const auto& entries = std::get<std::vector<HistoryEntry>>(result);
    if (!entry || entry > entries.size() || !claim || claim > entries[entry-1].answer.claims().size()) {
        errors << "В истории нет такой цитаты.\n";
        return 1;
    }
    const auto& selected = entries[entry-1];
    const auto& citation = selected.answer.claims()[claim-1].citation;
    const auto resolved = sources.resolveSource(selected.revision, citation);
    if (const auto* error = std::get_if<AppError>(&resolved)) {
        errors << describeError(*error) << '\n';
        return 1;
    }
    const auto& source = std::get<CitationSource>(resolved);
    printCitation(citation, output);
    output << "Сохранённый текст: " << source.blockText << "\nСимволы цитаты: "
           << source.highlight.first << "–" << source.highlight.last << '\n';
    if (source.copyStatus == CopyStatus::Available) {
        const auto path = source.copyPath.u8string();
        output << "Копия: " << std::string(path.begin(), path.end()) << '\n';
    } else if (source.copyStatus == CopyStatus::Legacy) {
        output << "Исходная копия для этой записи не сохранялась.\n";
    } else {
        output << "Копия недоступна или её содержимое изменилось.\n";
    }
    return 0;
}

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
    std::size_t ordinal = 0;
    for (const auto& entry : entries) {
        output << '\n' << ++ordinal << ". " << entry.timeUtc << " | " << entry.profile.modelId << " | ревизия "
               << entry.revision.number << (entry.outdated ? " (устарела)" : "") << '\n'
               << entry.question << '\n';
        printAnswer(entry.answer, entry.profile, output);
    }
    return 0;
}

int runPolicyCli(IKnowledgeStore& store, std::optional<DocumentId> document, std::optional<DocumentPolicy> policy,
                 std::ostream& output, std::ostream& errors) {
    if (policy) {
        const auto result = document ? store.setDocumentPolicy(*document, *policy) : store.setLibraryPolicy(*policy);
        if (const auto* error = std::get_if<AppError>(&result)) {
            errors << describeError(*error) << '\n';
            return 1;
        }
        if (document) {
            output << "Документ " << *document << ": ";
        } else {
            output << "Библиотека: ";
        }
        output << policyName(*policy) << '\n';
        return 0;
    }

    const auto result = store.snapshot();
    if (const auto* error = std::get_if<AppError>(&result)) {
        errors << describeError(*error) << '\n';
        return 1;
    }
    const auto& library = std::get<LibrarySnapshot>(result);
    output << "Библиотека: " << policyName(library.policy) << '\n';
    for (const auto& doc : library.documents) {
        output << doc.id() << " | " << doc.source() << " | " << policyName(doc.policy()) << '\n';
    }
    return 0;
}

int runCli(IndexingService& index, RagService& rag, std::span<const std::filesystem::path> files,
           std::string question, const ModelProfile& profile, std::ostream& output, std::ostream& errors) {
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

    printAnswer(std::get<Answer>(result), profile, output);
    return status;
}

}  // namespace rag
