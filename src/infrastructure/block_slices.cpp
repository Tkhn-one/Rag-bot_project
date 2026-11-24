#include "rag/infrastructure/block_slices.hpp"
#include "sqlite_tokenizer.hpp"

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
int markToken(void* output, int, const char*, int, int firstByte, int pastLastByte) {
    auto& cuts = *static_cast<std::vector<bool>*>(output);
    std::fill(cuts.begin() + firstByte + 1, cuts.begin() + pastLastByte, false);
    return SQLITE_OK;
}

Result<std::vector<bool>> wordBoundaries(std::string_view text, sqlite3* db) {
    // The prototype's in-memory store also uses the built-in unicode61 rules.
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> temporary(nullptr, sqlite3_close);
    if (!db) {
        const auto status = sqlite3_open(":memory:", &db);
        temporary.reset(db);
        if (status != SQLITE_OK) return AppError::StoreUnavailable;
    }
    std::vector<bool> cuts(text.size() + 1, true);
    if (!sql::tokenizeUnicode61(db, text, FTS5_TOKENIZE_DOCUMENT, &cuts, markToken)) return AppError::StoreWriteFailed;
    return cuts;
}
}  // namespace

Result<std::vector<BlockSlice>> splitBlock(std::string_view block, const IEmbeddingProvider* tokenizer, sqlite3* db) {
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
    const auto fitPrefix = [&](std::size_t start, std::size_t stop) {
        return fittingLength(stop - start, tokenBudget, [&](auto n) { return count(slice(start, start + n)); });
    };
    std::vector<BlockSlice> result;
    std::vector<bool> cuts;
    std::size_t covered = 0;
    for (std::size_t start = 0; start < length;) {
        const auto width = fitPrefix(start, length);
        if (const auto* error = std::get_if<AppError>(&width)) return *error;
        const auto limit = start + std::get<std::size_t>(width);
        if (start == 0 && limit == length) return std::vector<BlockSlice>{{block, {}}};
        if (cuts.empty()) {
            auto boundaries = wordBoundaries(block, db);
            if (const auto* error = std::get_if<AppError>(&boundaries)) return *error;
            cuts = std::get<std::vector<bool>>(std::move(boundaries));
        }
        const bool startsInsideTerm = !cuts[offsets[start]];
        auto end = startsInsideTerm ? start : limit;
        while (end > covered) {
            if (cuts[offsets[end]]) {
                if (end == limit) break;
                const auto tokens = count(slice(start, end));
                if (const auto* error = std::get_if<AppError>(&tokens)) return *error;
                if (std::get<std::size_t>(tokens) <= tokenBudget) break;
            }
            --end;
        }
        if (end <= covered) {
            if (start < covered) {
                start = covered;  // Discard overlap before splitting an oversize term.
                continue;
            }
            if (limit == start) return AppError::EmbeddingFailed;
            // Isolate an oversize term, including its last piece. Its neighbours
            // remain independently searchable even if re-tokenizing changes the budget.
            end = start + 1;
            while (end < limit && !cuts[offsets[end]]) ++end;
            if (end != limit) {
                const auto remainder = fitPrefix(start, end);
                if (const auto* error = std::get_if<AppError>(&remainder)) return *error;
                end = start + std::get<std::size_t>(remainder);
                if (end == start) return AppError::EmbeddingFailed;
            }
        }
        const bool wholeTerms = !startsInsideTerm && cuts[offsets[end]];
        result.push_back({slice(start, end), CharacterRange{start + 1, end}, wholeTerms});
        covered = end;
        if (end == length) break;
        if (!wholeTerms) {
            start = end;
            continue;
        }
        const auto overlap = fittingLength(end - start - 1, overlapBudget, [&](auto n) { return count(slice(end - n, end)); });
        if (const auto* error = std::get_if<AppError>(&overlap)) return *error;
        start = end - std::get<std::size_t>(overlap);
        while (start < end && !cuts[offsets[start]]) ++start;
        // Moving to a word boundary can change the model's subword tokenization.
        while (start < end) {
            const auto tokens = count(slice(start, end));
            if (const auto* error = std::get_if<AppError>(&tokens)) return *error;
            if (std::get<std::size_t>(tokens) <= overlapBudget) break;
            do { ++start; } while (start < end && !cuts[offsets[start]]);
        }
    }
    return result;
}
}  // namespace rag
