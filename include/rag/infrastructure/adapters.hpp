#pragma once

#include "rag/application/ports.hpp"

namespace rag {

class TxtReader final : public IDocumentReader {
public:
    Result<std::string> read(const std::filesystem::path& path) const override;
};

class InMemoryKnowledgeStore final : public IKnowledgeStore {
public:
    void replace(IndexedDocument document) override {
        document_ = std::move(document);
    }

    const IndexedDocument* current() const noexcept override {
        return document_ ? &*document_ : nullptr;
    }

private:
    std::optional<IndexedDocument> document_;
};

class KeywordRetriever final : public IRetriever {
public:
    std::vector<Chunk> retrieve(const Question& question,
                                std::span<const Chunk> chunks) const override;
};

class MockProvider final : public ILlmProvider {
public:
    Result<ProposedCitation> generate(const Question&,
                                       std::span<const Chunk> evidence) override {
        if (evidence.empty()) {
            return AppError::InvalidCitation;
        }
        return ProposedCitation{evidence.front().line(), evidence.front().text()};
    }
};

}  // namespace rag
