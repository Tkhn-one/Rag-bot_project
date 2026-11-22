#pragma once

#include "rag/domain/model.hpp"

#include <filesystem>
#include <string_view>

namespace rag {

bool hasExtension(const std::filesystem::path& path, std::wstring_view expected);
Result<std::string> readLocalFile(const std::filesystem::path& path, std::size_t maxBytes);

}  // namespace rag
