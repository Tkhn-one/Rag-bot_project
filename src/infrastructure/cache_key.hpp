#pragma once

#include "rag/application/ports.hpp"

#include <array>

namespace rag {

using CacheDigest = std::array<unsigned char, 32>;
std::optional<CacheDigest> sha256(std::string_view bytes);
std::optional<CacheDigest> cacheKey(const GenerationRequest& request);

}  // namespace rag
