#pragma once

#include "rag/application/ports.hpp"

namespace rag {

class TxtReader final : public IDocumentReader {
public:
    Result<std::string> read(const std::filesystem::path& path) const override;
};

class InMemoryKnowledgeStore final : public IKnowledgeStore {
public:
    void append(IndexedDocument document) override {
        documents_.push_back(std::move(document));
    }

    std::span<const IndexedDocument> documents() const noexcept override {
        return documents_;
    }

    const IndexedDocument* current() const noexcept {
        return documents_.empty() ? nullptr : &documents_.back();
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
        return ProposedCitation{chunk.line(), chunk.text(), chunk.documentId()};
    }
};

}  // namespace rag
