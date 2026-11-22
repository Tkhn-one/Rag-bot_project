#include "local_file.hpp"

#include <algorithm>
#include <fstream>

namespace rag {

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
