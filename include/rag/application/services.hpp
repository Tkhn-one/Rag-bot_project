#pragma once

#include "rag/application/ports.hpp"

namespace rag {

Result<CharacterRange> locateQuote(std::string_view block, const Citation& citation);

class IndexingService {
public:
    IndexingService(const IDocumentReader& reader, IKnowledgeStore& store)
        : reader_(reader), store_(store) {}

    Result<ImportReport> import(const std::filesystem::path& path);
    Result<ImportReport> update(DocumentId id, const std::filesystem::path &path, std::stop_token stop = {});

  private:
    Result<ParsedSource> parse(const std::filesystem::path& path) const;
    const IDocumentReader& reader_;
    IKnowledgeStore& store_;
};

class ValidatingLlmProvider final : public ILlmProvider {
public:
    explicit ValidatingLlmProvider(ILlmProvider& inner) : inner_(inner) {}
    ModelProfile profile() const override { return inner_.profile(); }
    Result<GenerationResult> generate(const GenerationRequest& request) override;

private:
    ILlmProvider& inner_;
};

class CachingLlmProvider final : public ILlmProvider {
public:
    CachingLlmProvider(ValidatingLlmProvider& validated, IAnswerCacheStore& cache)
        : validated_(validated), cache_(cache) {}
    ModelProfile profile() const override { return validated_.profile(); }
    Result<GenerationResult> generate(const GenerationRequest& request) override;

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
    Result<Answer> askWithProfile(const std::string& text, const Question& query, const ModelProfile& profile);

    const IKnowledgeStore& store_;
    const IRetriever& retriever_;
    ILlmProvider& provider_;
    IHistoryStore* history_;
};

}  // namespace rag
