#include "rag/application/services.hpp"

#include <algorithm>
#include <string_view>

namespace rag {
namespace {

void appendDocxLayout(std::string& layout, const DocxPosition& position, std::size_t textBytes) {
    layout += std::to_string(position.part.size()) + ':' + position.part + ':';
    if (const auto* paragraph = std::get_if<std::size_t>(&position.anchor)) {
        layout += "p" + std::to_string(*paragraph) + ':';
    } else {
        const auto& cell = std::get<TableCell>(position.anchor);
        layout += "t" + std::to_string(cell.table) + ',' + std::to_string(cell.row) + ',' +
                  std::to_string(cell.cell) + ':';
    }
    layout += std::to_string(textBytes) + ';';
}

auto findCitation(std::span<const Chunk> evidence, const ProposedCitation& citation) {
    return std::ranges::find_if(evidence, [&](const Chunk& chunk) {
        return chunk.documentId() == citation.documentId && chunk.line() == citation.line &&
               chunk.page() == citation.page && chunk.docx() == citation.docx &&
               chunk.text() == citation.quote;
    });
}

}  // namespace

Result<ImportReport> IndexingService::import(const std::filesystem::path& path) {
    auto file = reader_.read(path);
    if (const auto* error = std::get_if<AppError>(&file)) {
        return *error;
    }

    std::string content;
    auto& extracted = std::get<ExtractedDocument>(file);
    const bool isDocx = !extracted.sections.empty() && extracted.sections.front().docx.has_value();
    std::string positionLayout;
    std::vector<DocumentLine> lines;
    std::vector<std::size_t> pagesWithoutText;
    std::size_t line = 1;
    for (auto& section : extracted.sections) {
        if (section.docx) {
            if (positionLayout.empty()) {
                positionLayout = "docx:";
            } else {
                content += '\n';
            }
            appendDocxLayout(positionLayout, *section.docx, section.text.size());
            content += section.text;
            if (section.text.find_first_not_of(" \t\r\n\f\v") != std::string::npos) {
                lines.push_back({line++, std::move(section.text), {}, std::move(section.docx)});
            }
            continue;
        }
        if (section.page) {
            if (!positionLayout.empty()) {
                content += '\n';
            }
            // Page lengths distinguish PDF page boundaries even when the joined text is identical.
            positionLayout += std::to_string(section.text.size()) + ',';
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
        if (isDocx) {
            return AppError::DocxNoText;
        }
        return positionLayout.empty() ? AppError::EmptyDocument : AppError::PdfNoText;
    }

    const auto filename = path.filename().u8string();
    const auto saved = store_.save(std::string(filename.begin(), filename.end()), std::move(content),
                                  std::move(lines), DocumentPolicy::LocalOnly, std::move(positionLayout));
    if (const auto* error = std::get_if<AppError>(&saved)) {
        return *error;
    }
    return ImportReport{std::get<std::size_t>(saved), std::move(pagesWithoutText), extracted.incomplete};
}

Result<ProposedCitation> ValidatingLlmProvider::generate(const GenerationRequest& request) {
    auto result = inner_.generate(request);
    if (auto* citation = std::get_if<ProposedCitation>(&result)) {
        if (findCitation(request.evidence, *citation) == request.evidence.end()) {
            return AppError::InvalidCitation;
        }
        citation->fromCache = false;
    }
    return result;
}

Result<ProposedCitation> CachingLlmProvider::generate(const GenerationRequest& request) {
    const auto cached = cache_.lookup(request);
    if (const auto* error = std::get_if<AppError>(&cached)) {
        return *error;
    }
    if (const auto& ref = std::get<std::optional<ChunkReference>>(cached)) {
        const auto match = std::ranges::find_if(request.evidence, [&](const Chunk& chunk) {
            return chunk.documentId() == ref->documentId && chunk.line() == ref->line;
        });
        if (match == request.evidence.end()) {
            return AppError::StoreCorrupt;
        }
        return ProposedCitation::fromChunk(*match, true);
    }

    auto result = validated_.generate(request);
    if (const auto* citation = std::get_if<ProposedCitation>(&result)) {
        const auto saved = cache_.remember(request, {citation->documentId, citation->line});
        if (const auto* error = std::get_if<AppError>(&saved)) {
            return *error;
        }
    }
    return result;
}

Result<Answer> RagService::ask(std::string text) {
    const auto question = Question::fromText(text);
    if (const auto* error = std::get_if<AppError>(&question)) {
        return *error;
    }

    const auto snapshot = store_.snapshot();
    if (const auto* error = std::get_if<AppError>(&snapshot)) {
        return *error;
    }

    const auto& library = std::get<LibrarySnapshot>(snapshot);
    const auto& documents = library.documents;
    const auto profile = provider_.profile();
    if (profile.processing != ProcessingType::Local &&
        (library.policy != DocumentPolicy::AllowRemote ||
         !std::ranges::all_of(documents, [](const IndexedDocument& document) {
             return document.policy() == DocumentPolicy::AllowRemote;
         }))) {
        return AppError::RemoteProcessingDenied;
    }
    const auto complete = [&](Answer answer) -> Result<Answer> {
        if (history_) {
            const auto saved = history_->appendHistory(text, library.revision, profile, answer);
            if (const auto* error = std::get_if<AppError>(&saved)) {
                return *error;
            }
        }
        return answer;
    };
    if (documents.empty()) {
        return complete(Answer{});
    }

    const auto& query = std::get<Question>(question);
    const auto evidence = retriever_.retrieve(query, documents);
    if (evidence.empty()) {
        return complete(Answer{});
    }

    const auto proposed = provider_.generate({query, library.revision, profile, evidence});
    if (const auto* error = std::get_if<AppError>(&proposed)) {
        return *error;
    }

    const auto& citation = std::get<ProposedCitation>(proposed);
    const std::span<const Chunk> candidates = evidence;
    const auto match = findCitation(candidates, citation);
    if (match == candidates.end()) {
        return AppError::InvalidCitation;
    }

    const auto document = std::ranges::find(documents, match->documentId(), &IndexedDocument::id);
    if (document == documents.end()) {
        return AppError::InvalidCitation;
    }

    return complete(Answer(Citation(document->source(), *match), citation.fromCache));
}

}  // namespace rag
