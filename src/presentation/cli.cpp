#include "rag/presentation/cli.hpp"
#include "rag/application/services.hpp"

#include <ostream>
#include <string_view>

namespace rag {
namespace {

std::string_view describe(AppError error) {
    switch (error) {
    case AppError::UnsupportedFormat:
        return "Выберите файл с расширением .txt.";
    case AppError::FileUnavailable:
        return "Не удалось открыть или прочитать TXT-файл.";
    case AppError::FileTooLarge:
        return "Размер TXT-файла превышает 8 МиБ.";
    case AppError::InvalidEncoding:
        return "Не удалось прочитать кодировку. Сохраните файл в UTF-8.";
    case AppError::EmptyDocument:
        return "В TXT-файле нет текста.";
    case AppError::EmptyQuestion:
        return "Введите вопрос.";
    case AppError::InvalidCitation:
        return "Тестовый провайдер вернул неподтверждённую цитату.";
    }
    return "Не удалось выполнить запрос.";
}

}  // namespace

int runCli(IndexingService& index, RagService& rag, const std::filesystem::path& file,
           std::string question, std::ostream& output, std::ostream& errors) {
    const auto imported = index.import(file);
    if (const auto* error = std::get_if<AppError>(&imported)) {
        errors << describe(*error) << '\n';
        return 1;
    }

    const auto result = rag.ask(std::move(question));
    if (const auto* error = std::get_if<AppError>(&result)) {
        errors << describe(*error) << '\n';
        return 1;
    }

    const auto& citation = std::get<Answer>(result).citation();
    if (!citation) {
        output << "В документе не найдено подтверждения по словам вопроса.\n";
        return 0;
    }

    output << "Найденный фрагмент (тестовый провайдер)\n"
           << "«" << citation->quote() << "»\n"
           << "Источник: " << citation->source() << ", строка " << citation->line() << '\n';
    return 0;
}

}  // namespace rag
