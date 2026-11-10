#pragma once

#include "rag/application/ports.hpp"

namespace rag {

class IndexingService {
public:
    IndexingService(const IDocumentReader& reader, IKnowledgeStore& store)
        : reader_(reader), store_(store) {}

    Result<std::size_t> import(const std::filesystem::path& path);

private:
    const IDocumentReader& reader_;
    IKnowledgeStore& store_;
};

class RagService {
public:
    RagService(const IKnowledgeStore& store, const IRetriever& retriever,
               ILlmProvider& provider)
        : store_(store), retriever_(retriever), provider_(provider) {}

    Result<Answer> ask(std::string question);

private:
    const IKnowledgeStore& store_;
    const IRetriever& retriever_;
    ILlmProvider& provider_;
};

}  // namespace rag
