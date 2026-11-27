#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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
    RemoteProcessingDenied,
    EmbeddingFailed,
    LocalGenerationDisabled,
    LocalModelUnavailable,
    LocalModelMismatch,
    LocalModelLoadFailed,
    LocalContextTooLarge,
    LocalGenerationFailed,
    SearchProfileMismatch,
    InvalidGeneration,
    RemoteContextTooLarge,
    ApiKeyMissing,
    ApiKeyUnavailable,
    RemoteUnauthorized,
    RemoteForbidden,
    RemoteRateLimited,
    RemoteServiceUnavailable,
    RemoteHttpError,
    RemoteNetworkError,
    RemoteTimeout,
    RemoteRefusal,
    RemoteIncomplete,
    LibraryBusy,
    Cancelled
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
    std::string generationParameters = "default";
    std::string responseFormat = "extractive-v1";
    std::string endpoint = "builtin";
    // Public configuration identifier rotated when credentials change. Never a secret.
    std::string credentialScope;

    bool operator==(const ModelProfile&) const = default;
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

// One-based, inclusive Unicode scalar positions within the extracted source block.
struct CharacterRange {
    std::size_t first, last;
    bool operator==(const CharacterRange&) const = default;
};

class Chunk {
public:
    Chunk(DocumentId documentId, std::size_t line, std::string text,
          std::optional<std::size_t> page = {}, std::optional<DocxPosition> docx = {},
          std::size_t chunkId = 0, std::optional<CharacterRange> characters = {})
        : documentId_(documentId), line_(line), text_(std::move(text)), page_(page),
          docx_(std::move(docx)), chunkId_(chunkId ? chunkId : line), characters_(characters) {}

    std::size_t chunkId() const noexcept { return chunkId_; }
    std::optional<CharacterRange> characters() const noexcept { return characters_; }

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
    std::size_t chunkId_;
    std::optional<CharacterRange> characters_;
};

class Question {
public:
    static Result<Question> fromText(std::string text) {
        const auto start = text.find_first_not_of(" \t\r\n\f\v");
        if (start == std::string::npos) {
            return AppError::EmptyQuestion;
        }
        const auto end = text.find_last_not_of(" \t\r\n\f\v");
        std::string normalized;
        normalized.reserve(end - start + 1);
        for (auto i = start; i <= end; ++i) {
            if (text[i] == '\r') {
                normalized += '\n';
                if (i < end && text[i + 1] == '\n') {
                    ++i;
                }
            } else {
                normalized += text[i];
            }
        }
        return Question(std::move(normalized));
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
    std::optional<CharacterRange> characters() const noexcept { return chunk_.characters(); }
    std::optional<std::size_t> page() const noexcept { return chunk_.page(); }
    const std::optional<DocxPosition>& docx() const noexcept { return chunk_.docx(); }
    const std::string& quote() const noexcept { return chunk_.text(); }

private:
    std::string source_;
    Chunk chunk_;
};

struct CitedClaim {
    std::string text;
    Citation citation;
};

enum class AnswerStatus { Answered, InsufficientEvidence };

class Answer {
public:
    Answer() = default;
    explicit Answer(std::vector<CitedClaim> claims, bool fromCache = false,
                    std::optional<std::vector<DocumentId>> context = {})
        : claims_(std::move(claims)), fromCache_(fromCache), context_(std::move(context)) {}
    const std::optional<std::vector<DocumentId>> &context() const noexcept { return context_; }

    AnswerStatus status() const noexcept {
        return claims_.empty() ? AnswerStatus::InsufficientEvidence : AnswerStatus::Answered;
    }
    const std::vector<CitedClaim>& claims() const noexcept { return claims_; }
    bool fromCache() const noexcept { return fromCache_; }

private:
    std::vector<CitedClaim> claims_;
    bool fromCache_ = false;
    std::optional<std::vector<DocumentId>> context_;
};

}  // namespace rag
