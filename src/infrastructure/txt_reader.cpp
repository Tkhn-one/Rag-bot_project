#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/utf8.hpp"
#include "local_file.hpp"

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

Result<ExtractedDocument> TxtReader::read(const std::filesystem::path& path) const {
    if (!hasExtension(path, L".txt")) {
        return AppError::UnsupportedFormat;
    }

    auto file = readLocalFile(path, maxFileBytes);
    if (const auto* error = std::get_if<AppError>(&file)) {
        return *error;
    }
    auto text = decodeTxt(std::get<std::string>(std::move(file)));
    if (!text || text->find('\0') != std::string::npos) {
        return AppError::InvalidEncoding;
    }
    std::vector<TextSection> sections;
    sections.push_back({std::move(*text)});
    return ExtractedDocument{std::move(sections)};
}

}  // namespace rag
