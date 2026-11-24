#pragma once

#include "rag/application/ports.hpp"

namespace rag {

struct BlockSlice {
    std::string_view text;
    std::optional<CharacterRange> characters;
};

// Views point into the original block. Each endpoint is a UTF-8 character boundary.
Result<std::vector<BlockSlice>> splitBlock(std::string_view block, const IEmbeddingProvider* tokenizer);

}  // namespace rag
