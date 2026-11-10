#pragma once

#include <cstddef>
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
    InvalidCitation
};

template <typename T>
using Result = std::variant<T, AppError>;

class Chunk {
public:
    Chunk(std::size_t line, std::string text)
        : line_(line), text_(std::move(text)) {}

    // Within one imported document, the line number also identifies the chunk.
    std::size_t line() const noexcept { return line_; }
    const std::string& text() const noexcept { return text_; }

private:
    std::size_t line_;
    std::string text_;
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
    std::size_t line() const noexcept { return chunk_.line(); }
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
