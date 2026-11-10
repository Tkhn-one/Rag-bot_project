#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/utf8.hpp"

#include <unordered_set>

namespace rag {
namespace {

wchar_t fold(wchar_t letter) {
    if ((letter >= L'A' && letter <= L'Z') || (letter >= L'А' && letter <= L'Я')) {
        return static_cast<wchar_t>(letter + 32);
    }
    return letter == L'Ё' ? L'ё' : letter;
}

bool isWordCharacter(wchar_t letter) {
    return (letter >= L'a' && letter <= L'z') ||
           (letter >= L'а' && letter <= L'я') || letter == L'ё' ||
           (letter >= L'0' && letter <= L'9');
}

std::unordered_set<std::wstring> words(std::string_view text) {
    std::unordered_set<std::wstring> result;
    const auto decoded = decodeUtf8(text);
    if (!decoded) {
        return result;
    }

    std::wstring word;
    for (const auto character : *decoded) {
        const auto letter = fold(character);
        if (isWordCharacter(letter)) {
            word += letter;
        } else if (!word.empty()) {
            result.insert(word);
            word.clear();
        }
    }
    if (!word.empty()) {
        result.insert(std::move(word));
    }
    return result;
}

}  // namespace

std::vector<Chunk> KeywordRetriever::retrieve(const Question& question,
                                               std::span<const Chunk> chunks) const {
    const auto terms = words(question.text());
    if (terms.empty()) {
        return {};
    }

    const Chunk* best = nullptr;
    std::size_t bestScore = 0;
    for (const auto& chunk : chunks) {
        const auto candidates = words(chunk.text());
        std::size_t score = 0;
        for (const auto& term : terms) {
            if (candidates.contains(term)) {
                ++score;
            }
        }
        if (score > bestScore) {
            best = &chunk;
            bestScore = score;
        }
    }

    if (best) {
        return {*best};
    }
    return {};
}

}  // namespace rag
