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

bool isContinuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xc0) == 0x80;
}

std::size_t characterCount(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char byte) {
        return !isContinuation(byte);
    }));
}

Result<Answer> resolveAnswer(const GenerationRequest& request, const GenerationResult& result) {
    const auto* answered = std::get_if<Answered>(&result.value);
    if (!answered) {
        return Answer({}, result.fromCache);
    }
    if (answered->claims.empty()) {
        return AppError::InvalidGeneration;
    }

    std::vector<CitedClaim> claims;
    claims.reserve(answered->claims.size());
    for (const auto& claim : answered->claims) {
        if (claim.text.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
            return AppError::InvalidGeneration;
        }
        const auto index = contextChunkIndex(claim.chunkId, request.evidence.size());
        if (!index || claim.quote.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
            return AppError::InvalidCitation;
        }
        const auto& chunk = request.evidence[*index];
        const std::string_view text = chunk.text();
        const auto begin = text.find(claim.quote);
        if (begin == std::string_view::npos) {
            return AppError::InvalidCitation;
        }
        const auto end = begin + claim.quote.size();
        if (isContinuation(text[begin]) || (end < text.size() && isContinuation(text[end]))) {
            return AppError::InvalidCitation;
        }

        const auto document = std::ranges::find(request.documents, chunk.documentId(), &IndexedDocument::id);
        if (document == request.documents.end()) {
            return AppError::InvalidCitation;
        }
        auto range = chunk.characters();
        if (claim.quote.size() != text.size()) {
            if (text.find(claim.quote, begin + 1) == std::string_view::npos) {
                const auto first = (range ? range->first : 1) + characterCount(text.substr(0, begin));
                range = CharacterRange{first, first + characterCount(claim.quote) - 1};
            } else if (!range) {
                range = CharacterRange{1, characterCount(text)};
            }
        }
        claims.push_back({claim.text, Citation(document->source(),
            Chunk(chunk.documentId(), chunk.line(), claim.quote, chunk.page(), chunk.docx(), chunk.chunkId(), range))});
    }
    return Answer(std::move(claims), result.fromCache);
}

}  // namespace

Result<CharacterRange> locateQuote(std::string_view block, const Citation& citation) {
    if (citation.quote().empty()) return AppError::InvalidCitation;
    // Restrict the search to the cited slice before locating the verbatim quote.
    std::size_t start = 0, end = block.size();
    if (const auto range = citation.characters()) {
        if (!range->first || range->last < range->first) return AppError::InvalidCitation;
        std::size_t characters = 0;
        for (std::size_t i = 0; i < block.size(); ++i) {
            if (isContinuation(block[i])) continue;
            ++characters;
            if (characters == range->first) start = i;
            if (characters > range->last) { end = i; break; }
        }
        if (characters < range->last) return AppError::InvalidCitation;
    }
    const auto match = block.substr(start, end - start).find(citation.quote());
    if (match == block.npos) return AppError::InvalidCitation;
    const auto first = 1 + characterCount(block.substr(0, start + match));
    return CharacterRange{first, first + characterCount(citation.quote()) - 1};
}

Result<ImportReport> IndexingService::import(const std::filesystem::path& path) {
    return store_.importSource(path, [this](const auto& copy) { return parse(copy); });
}

Result<ParsedSource> IndexingService::parse(const std::filesystem::path& path) const {
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

    return ParsedSource{std::move(content), std::move(positionLayout), std::move(lines),
                        {0, std::move(pagesWithoutText), extracted.incomplete}};
}

Result<ImportReport> IKnowledgeStore::importSource(const std::filesystem::path& path, const ParseSource& parse) {
    auto parsed = parse(path);
    if (auto* error = std::get_if<AppError>(&parsed)) return *error;
    auto& source = std::get<ParsedSource>(parsed);
    const auto filename = path.filename().u8string();
    const auto saved = save(std::string(filename.begin(), filename.end()), std::move(source.content),
                            std::move(source.lines), DocumentPolicy::LocalOnly, std::move(source.layout));
    if (const auto* error = std::get_if<AppError>(&saved)) {
        return *error;
    }
    source.report.chunks = std::get<std::size_t>(saved);
    return std::move(source.report);
}

Result<GenerationResult> ValidatingLlmProvider::generate(const GenerationRequest& request) {
    const auto count = inner_.evidenceCount(request);
    if (const auto* error = std::get_if<AppError>(&count)) {
        return *error;
    }
    const auto size = std::get<std::size_t>(count);
    if (size > request.evidence.size()) {
        return AppError::InvalidGeneration;
    }
    auto sent = request;
    sent.evidence = request.evidence.first(size);
    auto result = inner_.generate(sent);
    if (auto* generated = std::get_if<GenerationResult>(&result)) {
        generated->fromCache = false;
        const auto answer = resolveAnswer(sent, *generated);
        if (const auto* error = std::get_if<AppError>(&answer)) {
            return *error;
        }
    }
    return result;
}

Result<GenerationResult> CachingLlmProvider::generate(const GenerationRequest& request) {
    auto cached = cache_.lookup(request);
    if (const auto* error = std::get_if<AppError>(&cached)) {
        return *error;
    }
    if (auto& hit = std::get<std::optional<GenerationResult>>(cached)) {
        hit->fromCache = true;
        return std::move(*hit);
    }

    auto result = validated_.generate(request);
    if (const auto* generated = std::get_if<GenerationResult>(&result)) {
        const auto saved = cache_.remember(request, *generated);
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

    const auto& query = std::get<Question>(question);
    const auto profile = provider_.profile();
    if (profile.processing == ProcessingType::Remote) {
        return store_.runRemoteQuestion([&] { return askWithProfile(text, query, profile); });
    }
    return askWithProfile(text, query, profile);
}

Result<Answer> RagService::askWithProfile(const std::string& text, const Question& query,
                                         const ModelProfile& profile) {
    const auto snapshot = store_.snapshot();
    if (const auto* error = std::get_if<AppError>(&snapshot)) {
        return *error;
    }

    const auto& library = std::get<LibrarySnapshot>(snapshot);
    const auto& documents = library.documents;
    if (profile.processing == ProcessingType::Remote &&
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

    const auto retrieved = retriever_.retrieve(query, library);
    if (const auto* error = std::get_if<AppError>(&retrieved)) {
        return *error;
    }
    const auto& evidence = std::get<std::vector<Chunk>>(retrieved);
    if (evidence.empty()) {
        return complete(Answer{});
    }

    const GenerationRequest request{query, library.revision, profile, evidence, documents};
    const auto proposed = provider_.generate(request);
    if (const auto* error = std::get_if<AppError>(&proposed)) {
        return *error;
    }

    const auto& generated = std::get<GenerationResult>(proposed);
    auto answer = resolveAnswer(request, generated);
    if (const auto* error = std::get_if<AppError>(&answer)) {
        return generated.fromCache ? AppError::StoreCorrupt : *error;
    }

    return complete(std::get<Answer>(std::move(answer)));
}

}  // namespace rag
