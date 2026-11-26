#pragma once

#include "rag/domain/model.hpp"

#include <filesystem>
#include <memory>
#include <string_view>
#include <llama.h>

namespace rag {
using LlamaModel = std::unique_ptr<llama_model, decltype(&llama_model_free)>;
using LlamaContext = std::unique_ptr<llama_context, decltype(&llama_free)>;

Result<LlamaModel> loadVerifiedCpuModel(const std::filesystem::path& gguf, std::uintmax_t size,
                                       std::string_view sha256);
}  // namespace rag
