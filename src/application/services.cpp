#include "rag/application/services.hpp"

#include <algorithm>
#include <string_view>

namespace rag {

Result<std::size_t> IndexingService::import(const std::filesystem::path& path) {
    const auto file = reader_.read(path);
    if (const auto* error = std::get_if<AppError>(&file)) {
        return *error;
    }

    const auto filename = path.filename().u8string();
    IndexedDocument document{std::string(filename.begin(), filename.end()), {}};
    std::string_view remaining = std::get<std::string>(file);
    for (std::size_t line = 1; !remaining.empty(); ++line) {
        const auto end = remaining.find_first_of("\r\n");
        const auto text = remaining.substr(0, end);
        if (text.find_first_not_of(" \t\f\v") != std::string_view::npos) {
            document.chunks.emplace_back(line, std::string(text));
        }
        if (end == std::string_view::npos) {
            break;
        }
        const bool crlf = remaining.substr(end, 2) == "\r\n";
        remaining.remove_prefix(end + (crlf ? 2 : 1));
    }

    if (document.chunks.empty()) {
        return AppError::EmptyDocument;
    }

    const auto count = document.chunks.size();
    store_.replace(std::move(document));
    return count;
}

Result<Answer> RagService::ask(std::string text) {
    const auto question = Question::fromText(std::move(text));
    if (const auto* error = std::get_if<AppError>(&question)) {
        return *error;
    }

    const auto* document = store_.current();
    if (!document) {
        return Answer{};
    }

    const auto& query = std::get<Question>(question);
    const auto evidence = retriever_.retrieve(query, document->chunks);
    if (evidence.empty()) {
        return Answer{};
    }

    const auto proposed = provider_.generate(query, evidence);
    if (const auto* error = std::get_if<AppError>(&proposed)) {
        return *error;
    }

    const auto& citation = std::get<ProposedCitation>(proposed);
    const auto match = std::ranges::find_if(evidence, [&](const Chunk& chunk) {
        return chunk.line() == citation.line && chunk.text() == citation.quote;
    });
    if (match == evidence.end()) {
        return AppError::InvalidCitation;
    }

    return Answer(Citation(document->source, *match));
}

}  // namespace rag
