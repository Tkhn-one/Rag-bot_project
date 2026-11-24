#pragma once

#include "rag/application/ports.hpp"

struct sqlite3;

namespace rag {

struct BlockSlice {
    std::string_view text;
    std::optional<CharacterRange> characters;
    bool wholeTerms = true;
};

// Views point into the original block. Each endpoint is a UTF-8 character boundary.
Result<std::vector<BlockSlice>> splitBlock(std::string_view block, const IEmbeddingProvider* tokenizer,
                                          sqlite3* database = nullptr);

}  // namespace rag
