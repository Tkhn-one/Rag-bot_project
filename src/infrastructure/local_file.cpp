#include "local_file.hpp"
#include "rag/infrastructure/local_path.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <fstream>

namespace rag {

bool isLocalStoragePath(const std::filesystem::path& input, bool allowMissingFile) {
    std::error_code ec;
    const auto path = std::filesystem::absolute(input, ec).lexically_normal();
    if (ec || path.native().starts_with(L"\\\\") || path.native().find(L':', 2) != std::wstring::npos)
        return false;
    const auto drive = GetDriveTypeW(path.root_path().c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK) return false;
    auto prefix = path.root_path();
    for (const auto& part : path.relative_path()) {
        prefix /= part;
        const auto attrs = GetFileAttributesW(prefix.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const auto reason = GetLastError();
            return allowMissingFile && prefix == path && reason == ERROR_FILE_NOT_FOUND;
        }
        if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) return false;
    }
    return true;
}

bool hasExtension(const std::filesystem::path& path, std::wstring_view expected) {
    auto extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t letter) {
        if (letter >= L'A' && letter <= L'Z') {
            return static_cast<wchar_t>(letter + 32);
        }
        return letter;
    });
    return extension == expected;
}

Result<std::string> readLocalFile(const std::filesystem::path& path, std::size_t maxBytes) {
    std::error_code statusError;
    if (!std::filesystem::is_regular_file(path, statusError)) {
        return AppError::FileUnavailable;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return AppError::FileUnavailable;
    }

    std::string bytes;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > maxBytes - bytes.size()) {
            return AppError::FileTooLarge;
        }
        bytes.append(buffer, count);
    }
    if (file.bad() || !file.eof()) {
        return AppError::FileUnavailable;
    }
    return bytes;
}

}  // namespace rag
