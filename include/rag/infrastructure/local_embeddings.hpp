#pragma once

#include "rag/application/ports.hpp"
#include <memory>

namespace rag {
Result<std::unique_ptr<IEmbeddingProvider>> openLocalEmbeddings(const std::filesystem::path& gguf);
}  // namespace rag
