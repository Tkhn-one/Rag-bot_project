#include "rag/infrastructure/local_embeddings.hpp"

#ifdef RAG_LLAMA_EMBEDDINGS
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <llama.h>

#include <array>
#include <fstream>

namespace rag {
namespace {
constexpr auto weightsHash = "06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439";
constexpr int tokenLimit = 2048;

bool verifiedWeights(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != 639150592 || ec) return false;
    std::ifstream stream(path, std::ios::binary);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!stream || !BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)))
        return false;
    const std::unique_ptr<void, decltype(&BCryptDestroyHash)> owner(hash, BCryptDestroyHash);
    std::array<unsigned char, 65536> buffer;
    while (stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || stream.gcount() > 0) {
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), static_cast<ULONG>(stream.gcount()), 0))) return false;
    }
    std::array<unsigned char, 32> digest;
    if (stream.bad() || !stream.eof() || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), 32, 0))) return false;
    std::string hex;
    for (auto byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex == weightsHash;
}

struct Backend {
    Backend() {
        llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
        llama_backend_init();
    }
    ~Backend() { llama_backend_free(); }
};
using Model = std::unique_ptr<llama_model, decltype(&llama_model_free)>;
using Context = std::unique_ptr<llama_context, decltype(&llama_free)>;

class QwenEmbeddings final : public IEmbeddingProvider {
public:
    QwenEmbeddings(Model model, Context ctx) : model_(std::move(model)), ctx_(std::move(ctx)) {}
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
    Model model_;
    Context ctx_;
};
}  // namespace

Result<std::unique_ptr<IEmbeddingProvider>> openLocalEmbeddings(const std::filesystem::path& gguf) {
    std::error_code ec;
    const auto path = std::filesystem::absolute(gguf, ec);
    if (ec || path.native().starts_with(L"\\\\") || GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE ||
        !verifiedWeights(path)) return AppError::EmbeddingFailed;
    static Backend backend;
    const auto filename = path.u8string();
    auto options = llama_model_default_params();
    options.n_gpu_layers = 0;
    Model model(llama_model_load_from_file(reinterpret_cast<const char*>(filename.c_str()), options), llama_model_free);
    if (!model || llama_model_n_embd(model.get()) != 1024) return AppError::EmbeddingFailed;
    auto params = llama_context_default_params();
    params.n_ctx = params.n_batch = params.n_ubatch = tokenLimit;
    params.embeddings = true;
    params.pooling_type = LLAMA_POOLING_TYPE_LAST;
    Context ctx(llama_init_from_model(model.get(), params), llama_free);
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
