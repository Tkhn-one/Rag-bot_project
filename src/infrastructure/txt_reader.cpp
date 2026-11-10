#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/utf8.hpp"

#include <algorithm>
#include <fstream>

namespace rag {
namespace {

constexpr std::size_t maxFileBytes = 8 * 1024 * 1024;

std::optional<std::string> decodeTxt(std::string bytes) {
    const bool littleEndian = bytes.starts_with("\xFF\xFE");
    const bool bigEndian = bytes.starts_with("\xFE\xFF");
    if (littleEndian || bigEndian) {
        if (bytes.size() % 2 != 0) {
            return std::nullopt;
        }

        std::wstring text;
        text.reserve((bytes.size() - 2) / 2);
        for (std::size_t i = 2; i < bytes.size(); i += 2) {
            const auto first = static_cast<unsigned char>(bytes[i]);
            const auto second = static_cast<unsigned char>(bytes[i + 1]);
            const auto low = littleEndian ? first : second;
            const auto high = littleEndian ? second : first;
            text.push_back(static_cast<wchar_t>((high << 8) | low));
        }
        return encodeUtf8(text);
    }

    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.erase(0, 3);
    }
    if (!decodeUtf8(bytes)) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace

Result<std::string> TxtReader::read(const std::filesystem::path& path) const {
    auto extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t letter) {
        if (letter >= L'A' && letter <= L'Z') {
            return static_cast<wchar_t>(letter - L'A' + L'a');
        }
        return letter;
    });
    if (extension != L".txt") {
        return AppError::UnsupportedFormat;
    }

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
        if (count > maxFileBytes - bytes.size()) {
            return AppError::FileTooLarge;
        }
        bytes.append(buffer, count);
    }
    if (file.bad() || !file.eof()) {
        return AppError::FileUnavailable;
    }

    auto text = decodeTxt(std::move(bytes));
    if (!text || text->find('\0') != std::string::npos) {
        return AppError::InvalidEncoding;
    }
    return std::move(*text);
}

}  // namespace rag
