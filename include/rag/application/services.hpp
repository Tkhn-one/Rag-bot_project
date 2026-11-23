#pragma once

#include "rag/application/ports.hpp"

namespace rag {

struct ImportReport {
    std::size_t chunks;
    std::vector<std::size_t> pagesWithoutText;
    bool incomplete;
};

class IndexingService {
public:
    IndexingService(const IDocumentReader& reader, IKnowledgeStore& store)
        : reader_(reader), store_(store) {}

    Result<ImportReport> import(const std::filesystem::path& path);

private:
    const IDocumentReader& reader_;
    IKnowledgeStore& store_;
};

class ValidatingLlmProvider final : public ILlmProvider {
public:
    explicit ValidatingLlmProvider(ILlmProvider& inner) : inner_(inner) {}
    ModelProfile profile() const override { return inner_.profile(); }
    Result<ProposedCitation> generate(const GenerationRequest& request) override;

private:
    ILlmProvider& inner_;
};

class CachingLlmProvider final : public ILlmProvider {
public:
    CachingLlmProvider(ValidatingLlmProvider& validated, IAnswerCacheStore& cache)
        : validated_(validated), cache_(cache) {}
    ModelProfile profile() const override { return validated_.profile(); }
    Result<ProposedCitation> generate(const GenerationRequest& request) override;

private:
    ValidatingLlmProvider& validated_;
    IAnswerCacheStore& cache_;
};

class RagService {
public:
    RagService(const IKnowledgeStore& store, const IRetriever& retriever,
               ILlmProvider& provider, IHistoryStore* history = nullptr)
        : store_(store), retriever_(retriever), provider_(provider), history_(history) {}

    Result<Answer> ask(std::string question);

private:
    const IKnowledgeStore& store_;
    const IRetriever& retriever_;
    ILlmProvider& provider_;
    IHistoryStore* history_;
};

}  // namespace rag
