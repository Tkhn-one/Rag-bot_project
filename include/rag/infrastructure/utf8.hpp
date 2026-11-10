#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace rag {

std::optional<std::wstring> decodeUtf8(std::string_view text);
std::optional<std::string> encodeUtf8(std::wstring_view text);

}  // namespace rag
