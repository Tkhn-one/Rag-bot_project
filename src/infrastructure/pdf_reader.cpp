#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/utf8.hpp"
#include "local_file.hpp"

#include <fpdf_text.h>
#include <fpdfview.h>

#include <memory>
#include <mutex>
#include <type_traits>

namespace rag {
namespace {

constexpr std::size_t maxPdfBytes = 64 * 1024 * 1024;
constexpr std::size_t maxExtractedBytes = 32 * 1024 * 1024;

class PdfRuntime {
public:
    PdfRuntime() { FPDF_InitLibrary(); }
    ~PdfRuntime() { FPDF_DestroyLibrary(); }

    // PDFium calls must not overlap across threads.
    std::unique_lock<std::mutex> lock() { return std::unique_lock(mutex_); }

private:
    std::mutex mutex_;
};

Result<std::string> extractPage(FPDF_DOCUMENT document, int pageIndex, std::size_t remainingBytes) {
    const std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, decltype(&FPDF_ClosePage)>
        page(FPDF_LoadPage(document, pageIndex), FPDF_ClosePage);
    if (!page) {
        return AppError::PdfCorrupt;
    }
    const std::unique_ptr<std::remove_pointer_t<FPDF_TEXTPAGE>, decltype(&FPDFText_ClosePage)>
        textPage(FPDFText_LoadPage(page.get()), FPDFText_ClosePage);
    if (!textPage) {
        return AppError::PdfCorrupt;
    }
    const int count = FPDFText_CountChars(textPage.get());
    if (count < 0) {
        return AppError::PdfCorrupt;
    }
    if (count == 0) {
        return std::string{};
    }
    if (static_cast<std::size_t>(count) > maxExtractedBytes) {
        return AppError::PdfTextTooLarge;
    }
    std::vector<unsigned short> buffer(static_cast<std::size_t>(count) + 1);
    const int written = FPDFText_GetText(textPage.get(), 0, count, buffer.data());
    if (written <= 0 || static_cast<std::size_t>(written) > buffer.size()) {
        return AppError::PdfCorrupt;
    }
    std::wstring text(buffer.begin(), buffer.begin() + written - 1);
    std::erase(text, L'\0');
    constexpr auto whitespace = L" \t\r\n\f\v\u00a0\u1680\u2000\u2001\u2002\u2003"
                                L"\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029"
                                L"\u202f\u205f\u3000";
    if (text.find_first_not_of(whitespace) == std::wstring::npos) {
        return std::string{};
    }
    auto utf8 = encodeUtf8(text);
    if (!utf8) {
        return AppError::PdfCorrupt;
    }
    if (utf8->size() > remainingBytes) {
        return AppError::PdfTextTooLarge;
    }
    return std::move(*utf8);
}

}  // namespace

Result<ExtractedDocument> PdfReader::read(const std::filesystem::path& path) const {
    if (!hasExtension(path, L".pdf")) {
        return AppError::UnsupportedFormat;
    }
    const auto file = readLocalFile(path, maxPdfBytes);
    if (const auto* error = std::get_if<AppError>(&file)) {
        return *error;
    }

    const auto& bytes = std::get<std::string>(file);
    static PdfRuntime runtime;
    const auto lock = runtime.lock();
    const std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, decltype(&FPDF_CloseDocument)>
        document(FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr), FPDF_CloseDocument);
    if (!document) {
        const auto error = FPDF_GetLastError();
        return error == FPDF_ERR_PASSWORD || error == FPDF_ERR_SECURITY
                   ? AppError::PdfEncrypted : AppError::PdfCorrupt;
    }
    if (FPDF_GetSecurityHandlerRevision(document.get()) != -1) {
        return AppError::PdfEncrypted;
    }
    const int pages = FPDF_GetPageCount(document.get());
    if (pages <= 0) {
        return AppError::PdfCorrupt;
    }

    std::vector<TextSection> sections;
    std::size_t remainingBytes = maxExtractedBytes;
    for (int index = 0; index < pages; ++index) {
        auto extracted = extractPage(document.get(), index, remainingBytes);
        if (const auto* error = std::get_if<AppError>(&extracted)) {
            return *error;
        }
        auto text = std::get<std::string>(std::move(extracted));
        remainingBytes -= text.size();
        sections.push_back({std::move(text), static_cast<std::size_t>(index) + 1});
    }
    return ExtractedDocument{std::move(sections)};
}

}  // namespace rag
