#pragma once

#include "rag/domain/model.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace rag {

struct IndexedDocument {
    std::string source;
    std::vector<Chunk> chunks;
};

class IDocumentReader {
public:
    virtual ~IDocumentReader() = default;
    virtual Result<std::string> read(const std::filesystem::path& path) const = 0;
};

class IKnowledgeStore {
public:
    virtual ~IKnowledgeStore() = default;
    virtual void replace(IndexedDocument document) = 0;
    virtual const IndexedDocument* current() const noexcept = 0;
};

class IRetriever {
public:
    virtual ~IRetriever() = default;
    virtual std::vector<Chunk> retrieve(const Question& question,
                                        std::span<const Chunk> chunks) const = 0;
};

struct ProposedCitation {
    std::size_t line;
    std::string quote;
};

class ILlmProvider {
public:
    virtual ~ILlmProvider() = default;
    virtual Result<ProposedCitation> generate(const Question& question,
                                               std::span<const Chunk> evidence) = 0;
};

}  // namespace rag
