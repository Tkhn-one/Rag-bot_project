#include "rag/infrastructure/utf8.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <limits>

namespace rag {

std::optional<std::wstring> decodeUtf8(std::string_view text) {
    if (text.empty()) {
        return std::wstring{};
    }
    if (text.size() > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    const auto length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text.data(), length, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring result(size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            length, result.data(), size) != size) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> encodeUtf8(std::wstring_view text) {
    if (text.empty()) {
        return std::string{};
    }
    if (text.size() > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    const auto length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        return std::nullopt;
    }
    std::string result(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            length, result.data(), size, nullptr, nullptr) != size) {
        return std::nullopt;
    }
    return result;
}

}  // namespace rag
