#pragma once

#include <filesystem>

namespace rag {
// Reject network drives, UNC/device paths and reparse points before opening a
// local model or library. A newly created database may have a missing leaf.
bool isLocalStoragePath(const std::filesystem::path& path, bool allowMissingFile = false);
}  // namespace rag
