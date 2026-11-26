#include "rag/infrastructure/local_embeddings.hpp"

#ifdef RAG_LLAMA_EMBEDDINGS
#include "llama_runtime.hpp"

namespace rag {
namespace {
constexpr auto weightsHash = "06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439";
constexpr int tokenLimit = 2048;

class QwenEmbeddings final : public IEmbeddingProvider {
public:
    QwenEmbeddings(LlamaModel model, LlamaContext ctx) : model_(std::move(model)), ctx_(std::move(ctx)) {}
    std::string profileId() const override {
        return std::string("qwen3:b6500:last:eos:l2:ctx2048:retrieval-en-v1:") + weightsHash;
    }
    std::size_t dimension() const override { return 1024; }
    Result<std::size_t> documentTokenCount(std::string_view text) const override {
        if (!std::in_range<int32_t>(text.size())) return AppError::EmbeddingFailed;
        const auto* vocab = llama_model_get_vocab(model_.get());
        const auto needed = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), nullptr, 0, true, false);
        if (needed >= 0 || needed == INT32_MIN) return AppError::EmbeddingFailed;
        return static_cast<std::size_t>(-needed) + (llama_vocab_get_add_eos(vocab) ? 0 : 1);
    }
    Result<std::vector<float>> embed(std::string_view text, EmbeddingInput input) const override {
        std::string prompt;
        if (input == EmbeddingInput::Query)
            prompt = "Instruct: Given a search query, retrieve relevant passages that answer the query\nQuery: ";
        prompt += text;
        if (!std::in_range<int32_t>(prompt.size())) return AppError::EmbeddingFailed;
        const auto* vocab = llama_model_get_vocab(model_.get());
        std::vector<llama_token> tokens(tokenLimit);
        int count = llama_tokenize(vocab, prompt.data(), static_cast<int32_t>(prompt.size()),
                                   tokens.data(), tokenLimit, true, false);
        if (count <= 0) return AppError::EmbeddingFailed;
        if (tokens[count - 1] != llama_vocab_eos(vocab)) {
            if (count == tokenLimit) return AppError::EmbeddingFailed;
            tokens[count++] = llama_vocab_eos(vocab);
        }
        llama_memory_clear(llama_get_memory(ctx_.get()), true);
        auto batch = llama_batch_get_one(tokens.data(), count);
        if (llama_decode(ctx_.get(), batch) != 0) return AppError::EmbeddingFailed;
        const float* values = llama_get_embeddings_seq(ctx_.get(), 0);
        if (!values) return AppError::EmbeddingFailed;
        return std::vector<float>(values, values + dimension());
    }
private:
    LlamaModel model_;
    LlamaContext ctx_;
};
}  // namespace

Result<std::unique_ptr<IEmbeddingProvider>> openLocalEmbeddings(const std::filesystem::path& gguf) {
    auto loaded = loadVerifiedCpuModel(gguf, 639150592, weightsHash);
    if (std::holds_alternative<AppError>(loaded)) return AppError::EmbeddingFailed;
    auto model = std::get<LlamaModel>(std::move(loaded));
    if (llama_model_n_embd(model.get()) != 1024) return AppError::EmbeddingFailed;
    auto params = llama_context_default_params();
    params.n_ctx = params.n_batch = params.n_ubatch = tokenLimit;
    params.embeddings = true;
    params.pooling_type = LLAMA_POOLING_TYPE_LAST;
    LlamaContext ctx(llama_init_from_model(model.get(), params), llama_free);
    if (!ctx) return AppError::EmbeddingFailed;
    return std::unique_ptr<IEmbeddingProvider>(new QwenEmbeddings(std::move(model), std::move(ctx)));
}
}  // namespace rag
#else
namespace rag {
Result<std::unique_ptr<IEmbeddingProvider>> openLocalEmbeddings(const std::filesystem::path&) {
    return AppError::EmbeddingFailed;
}
}  // namespace rag
#endif
