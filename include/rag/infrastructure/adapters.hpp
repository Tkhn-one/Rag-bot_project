#pragma once

#include "rag/application/ports.hpp"

#include <algorithm>

namespace rag {

class TxtReader final : public IDocumentReader {
public:
    Result<std::vector<TextSection>> read(const std::filesystem::path& path) const override;
};

class PdfReader final : public IDocumentReader {
public:
    Result<std::vector<TextSection>> read(const std::filesystem::path& path) const override;
};

class InMemoryKnowledgeStore final : public IKnowledgeStore {
public:
    Result<std::size_t> save(std::string source, std::string content,
                             std::vector<DocumentLine> lines, DocumentPolicy policy,
                             std::string pageLayout = {}) override {
        if (lines.empty()) {
            return AppError::StoreWriteFailed;
        }
        const auto duplicate = std::ranges::find_if(documents_, [&](const IndexedDocument& doc) {
            return doc.content() == content && doc.pageLayout() == pageLayout;
        });
        if (duplicate != documents_.end()) {
            return duplicate->chunks().size();
        }

        std::ranges::sort(lines, {}, &DocumentLine::number);
        if (lines.front().number == 0 ||
            std::ranges::adjacent_find(lines, {}, &DocumentLine::number) != lines.end()) {
            return AppError::StoreWriteFailed;
        }
        const DocumentId id = documents_.size() + 1;
        std::vector<Chunk> chunks;
        chunks.reserve(lines.size());
        for (auto& line : lines) {
            chunks.emplace_back(id, line.number, std::move(line.text), line.page);
        }
        documents_.emplace_back(id, std::move(source), std::move(content), std::move(chunks),
                                policy, std::move(pageLayout));
        return lines.size();
    }

    Result<std::vector<IndexedDocument>> documents() const override {
        return documents_;
    }

private:
    std::vector<IndexedDocument> documents_;
};

class KeywordRetriever final : public IRetriever {
public:
    std::vector<Chunk> retrieve(const Question& question,
                                std::span<const IndexedDocument> documents) const override;
};

class MockProvider final : public ILlmProvider {
public:
    Result<ProposedCitation> generate(const Question&,
                                       std::span<const Chunk> evidence) override {
        if (evidence.empty()) {
            return AppError::InvalidCitation;
        }
        const auto& chunk = evidence.front();
        return ProposedCitation{chunk.line(), chunk.text(), chunk.documentId(), chunk.page()};
    }
};

}  // namespace rag
