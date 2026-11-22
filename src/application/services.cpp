#include "rag/application/services.hpp"

#include <algorithm>
#include <string_view>

namespace rag {

Result<ImportReport> IndexingService::import(const std::filesystem::path& path) {
    auto file = reader_.read(path);
    if (const auto* error = std::get_if<AppError>(&file)) {
        return *error;
    }

    std::string content;
    std::string pageLayout;
    std::vector<DocumentLine> lines;
    std::vector<std::size_t> pagesWithoutText;
    std::size_t line = 1;
    for (const auto& section : std::get<std::vector<TextSection>>(file)) {
        if (section.page) {
            if (!pageLayout.empty()) {
                content += '\n';
            }
            // Page lengths distinguish PDF page boundaries even when the joined text is identical.
            pageLayout += std::to_string(section.text.size()) + ',';
        }
        content += section.text;

        const auto previousCount = lines.size();
        auto remaining = std::string_view(section.text);
        while (!remaining.empty()) {
            const auto end = remaining.find_first_of("\r\n");
            const auto text = remaining.substr(0, end);
            if (text.find_first_not_of(" \t\f\v") != std::string_view::npos) {
                lines.push_back({line, std::string(text), section.page});
            }
            ++line;
            if (end == std::string_view::npos) {
                break;
            }
            const bool crlf = remaining.substr(end, 2) == "\r\n";
            remaining.remove_prefix(end + (crlf ? 2 : 1));
        }
        if (section.page && lines.size() == previousCount) {
            pagesWithoutText.push_back(*section.page);
        }
    }

    if (lines.empty()) {
        return pageLayout.empty() ? AppError::EmptyDocument : AppError::PdfNoText;
    }

    const auto filename = path.filename().u8string();
    const auto saved = store_.save(std::string(filename.begin(), filename.end()), std::move(content),
                                  std::move(lines), DocumentPolicy::LocalOnly, std::move(pageLayout));
    if (const auto* error = std::get_if<AppError>(&saved)) {
        return *error;
    }
    return ImportReport{std::get<std::size_t>(saved), std::move(pagesWithoutText)};
}

Result<Answer> RagService::ask(std::string text) {
    const auto question = Question::fromText(std::move(text));
    if (const auto* error = std::get_if<AppError>(&question)) {
        return *error;
    }

    const auto snapshot = store_.documents();
    if (const auto* error = std::get_if<AppError>(&snapshot)) {
        return *error;
    }

    const auto& documents = std::get<std::vector<IndexedDocument>>(snapshot);
    if (documents.empty()) {
        return Answer{};
    }

    const auto& query = std::get<Question>(question);
    const auto evidence = retriever_.retrieve(query, documents);
    if (evidence.empty()) {
        return Answer{};
    }

    const auto proposed = provider_.generate(query, evidence);
    if (const auto* error = std::get_if<AppError>(&proposed)) {
        return *error;
    }

    const auto& citation = std::get<ProposedCitation>(proposed);
    const auto match = std::ranges::find_if(evidence, [&](const Chunk& chunk) {
        return chunk.documentId() == citation.documentId && chunk.line() == citation.line &&
               chunk.page() == citation.page && chunk.text() == citation.quote;
    });
    if (match == evidence.end()) {
        return AppError::InvalidCitation;
    }

    const auto document = std::ranges::find(documents, match->documentId(), &IndexedDocument::id);
    if (document == documents.end()) {
        return AppError::InvalidCitation;
    }

    return Answer(Citation(document->source(), *match));
}

}  // namespace rag
