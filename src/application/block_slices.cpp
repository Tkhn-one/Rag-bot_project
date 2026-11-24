#include "rag/application/block_slices.hpp"

#include <algorithm>

namespace rag {
namespace {

constexpr std::size_t tokenBudget = 400;
constexpr std::size_t overlapBudget = 60;

// Probe bounded windows, even for a multi-megabyte line. Only keep a candidate after
// counting its actual substring: tokenization can change at a newly cut boundary.
template<class Count>
Result<std::size_t> fittingLength(std::size_t available, std::size_t budget, Count count) {
    std::size_t low = 0;
    auto high = std::min(available, budget);
    while (high > low) {
        const auto tokens = count(high);
        if (const auto* error = std::get_if<AppError>(&tokens)) return *error;
        if (std::get<std::size_t>(tokens) > budget) break;
        low = high;
        high = std::min(available, high * 2);
    }
    while (high > low + 1) {
        const auto mid = low + (high - low) / 2;
        const auto tokens = count(mid);
        if (const auto* error = std::get_if<AppError>(&tokens)) return *error;
        if (std::get<std::size_t>(tokens) <= budget) low = mid;
        else high = mid;
    }
    return low;
}
}  // namespace

Result<std::vector<BlockSlice>> splitBlock(std::string_view block, const IEmbeddingProvider* tokenizer) {
    if (block.empty() || !std::in_range<std::uint32_t>(block.size())) return AppError::StoreWriteFailed;
    // All readers supply validated UTF-8. The table translates scalar positions to byte offsets.
    std::vector<std::uint32_t> offsets;
    for (std::size_t i = 0; i < block.size(); ++i) {
        if ((static_cast<unsigned char>(block[i]) & 0xc0) != 0x80) offsets.push_back(static_cast<std::uint32_t>(i));
    }
    offsets.push_back(static_cast<std::uint32_t>(block.size()));
    const auto length = offsets.size() - 1;
    const auto slice = [&](std::size_t first, std::size_t last) {
        return block.substr(offsets[first], offsets[last] - offsets[first]);
    };
    const auto count = [&](std::string_view text) -> Result<std::size_t> {
        return tokenizer ? tokenizer->documentTokenCount(text) : Result<std::size_t>(text.size());
    };
    std::vector<BlockSlice> result;
    std::size_t covered = 0;
    for (std::size_t start = 0; start < length;) {
        const auto width = fittingLength(length - start, tokenBudget, [&](auto n) { return count(slice(start, start + n)); });
        if (const auto* error = std::get_if<AppError>(&width)) return *error;
        const auto end = start + std::get<std::size_t>(width);
        if (end <= covered) {
            // If re-tokenizing the overlap leaves no room, prefer forward progress to overlap.
            if (start == covered) return AppError::EmbeddingFailed;
            start = covered;
            continue;
        }
        if (start == 0 && end == length) return std::vector<BlockSlice>{{block, {}}};
        result.push_back({slice(start, end), CharacterRange{start + 1, end}});
        covered = end;
        if (end == length) break;
        const auto overlap = fittingLength(end - start - 1, overlapBudget, [&](auto n) { return count(slice(end - n, end)); });
        if (const auto* error = std::get_if<AppError>(&overlap)) return *error;
        start = end - std::get<std::size_t>(overlap);
    }
    return result;
}
}  // namespace rag
