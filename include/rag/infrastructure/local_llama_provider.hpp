#pragma once

#include "rag/application/ports.hpp"

#include <memory>

namespace rag {
inline constexpr int localOutputLimit = 1024;
Result<std::unique_ptr<ILlmProvider>> openLocalLlama(const std::filesystem::path& gguf,
                                                    int outputTokens = localOutputLimit);
}  // namespace rag
