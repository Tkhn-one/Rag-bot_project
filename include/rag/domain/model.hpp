#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace rag {

enum class AppError {
    UnsupportedFormat,
    FileUnavailable,
    FileTooLarge,
    InvalidEncoding,
    EmptyDocument,
    EmptyQuestion,
    InvalidCitation,
    StoreReadFailed,
    StoreWriteFailed,
    StoreUnavailable,
    StoreCorrupt,
    StoreSchemaUnsupported,
    StoreMigrationFailed,
    PdfCorrupt,
    PdfEncrypted,
    PdfNoText,
    PdfTextTooLarge,
    DocxCorrupt,
    DocxNoText,
    DocxLimitExceeded,
    RemoteProcessingDenied
};

template <typename T>
using Result = std::variant<T, AppError>;

using DocumentId = std::size_t;

enum class DocumentPolicy { LocalOnly, AllowRemote };

struct IndexRevision {
    std::string libraryId;
    std::uint64_t number = 0;

    bool operator==(const IndexRevision&) const = default;
};

enum class ProcessingType { Local, Remote };

struct ModelProfile {
    ProcessingType processing;
    std::string modelId;
};

struct TableCell {
    std::size_t table, row, cell;

    bool operator==(const TableCell&) const = default;
};

struct DocxPosition {
    std::string part;
    std::variant<std::size_t, TableCell> anchor;

    bool operator==(const DocxPosition&) const = default;
};

class Chunk {
public:
    Chunk(DocumentId documentId, std::size_t line, std::string text,
          std::optional<std::size_t> page = {}, std::optional<DocxPosition> docx = {})
        : documentId_(documentId), line_(line), text_(std::move(text)), page_(page),
          docx_(std::move(docx)) {}

    DocumentId documentId() const noexcept { return documentId_; }
    std::size_t line() const noexcept { return line_; }
    std::optional<std::size_t> page() const noexcept { return page_; }
    const std::optional<DocxPosition>& docx() const noexcept { return docx_; }
    const std::string& text() const noexcept { return text_; }

private:
    DocumentId documentId_;
    std::size_t line_;
    std::string text_;
    std::optional<std::size_t> page_;
    std::optional<DocxPosition> docx_;
};

class Question {
public:
    static Result<Question> fromText(std::string text) {
        const auto start = text.find_first_not_of(" \t\r\n\f\v");
        if (start == std::string::npos) {
            return AppError::EmptyQuestion;
        }
        const auto end = text.find_last_not_of(" \t\r\n\f\v");
        return Question(text.substr(start, end - start + 1));
    }

    const std::string& text() const noexcept { return text_; }

private:
    explicit Question(std::string text) : text_(std::move(text)) {}
    std::string text_;
};

class Citation {
public:
    Citation(std::string source, Chunk chunk)
        : source_(std::move(source)), chunk_(std::move(chunk)) {}

    const std::string& source() const noexcept { return source_; }
    DocumentId documentId() const noexcept { return chunk_.documentId(); }
    std::size_t line() const noexcept { return chunk_.line(); }
    std::optional<std::size_t> page() const noexcept { return chunk_.page(); }
    const std::optional<DocxPosition>& docx() const noexcept { return chunk_.docx(); }
    const std::string& quote() const noexcept { return chunk_.text(); }

private:
    std::string source_;
    Chunk chunk_;
};

// The prototype's answer is extractive: absence of a citation means
// that the document contained no matching evidence.
class Answer {
public:
    Answer() = default;
    explicit Answer(Citation citation) : citation_(std::move(citation)) {}

    const std::optional<Citation>& citation() const noexcept { return citation_; }

private:
    std::optional<Citation> citation_;
};

}  // namespace rag
