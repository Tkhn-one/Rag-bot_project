#pragma once

#include "rag/domain/model.hpp"

#include <filesystem>
#include <span>
#include <vector>

namespace rag {

struct DocumentLine {
    std::size_t number;
    std::string text;
    std::optional<std::size_t> page = {};
    std::optional<DocxPosition> docx = {};
};

struct TextSection {
    std::string text;
    std::optional<std::size_t> page = {};
    std::optional<DocxPosition> docx = {};
};

struct ExtractedDocument {
    std::vector<TextSection> sections;
    bool incomplete = false;
};

class IndexedDocument {
public:
    IndexedDocument(DocumentId id, std::string source, std::string content,
                    std::vector<Chunk> chunks, DocumentPolicy policy = DocumentPolicy::LocalOnly,
                    std::string positionLayout = {})
        : id_(id), source_(std::move(source)), content_(std::move(content)),
          chunks_(std::move(chunks)), policy_(policy), positionLayout_(std::move(positionLayout)) {}

    DocumentId id() const noexcept { return id_; }
    const std::string& source() const noexcept { return source_; }
    const std::string& content() const noexcept { return content_; }
    std::span<const Chunk> chunks() const noexcept { return chunks_; }
    DocumentPolicy policy() const noexcept { return policy_; }
    const std::string& positionLayout() const noexcept { return positionLayout_; }

private:
    DocumentId id_;
    std::string source_;
    std::string content_;
    std::vector<Chunk> chunks_;
    DocumentPolicy policy_;
    std::string positionLayout_;
};

class IDocumentReader {
public:
    virtual ~IDocumentReader() = default;
    // TXT returns one unnumbered section. PDF returns pages; DOCX returns located blocks.
    virtual Result<ExtractedDocument> read(const std::filesystem::path& path) const = 0;
};

class IKnowledgeStore {
public:
    virtual ~IKnowledgeStore() = default;

    // Publish the document and its lines together. Line numbers must be positive and unique.
    // Position layout is empty for TXT. For equal text and layout, return the existing chunk count.
    virtual Result<std::size_t> save(std::string source, std::string content,
                                     std::vector<DocumentLine> lines, DocumentPolicy policy,
                                     std::string positionLayout = {}) = 0;

    // Return an owning snapshot ordered by document ID and line number.
    virtual Result<std::vector<IndexedDocument>> documents() const = 0;
};

class IRetriever {
public:
    virtual ~IRetriever() = default;
    virtual std::vector<Chunk> retrieve(const Question& question,
                                        std::span<const IndexedDocument> documents) const = 0;
};

struct ProposedCitation {
    std::size_t line;
    std::string quote;
    DocumentId documentId = 0;
    std::optional<std::size_t> page = {};
    std::optional<DocxPosition> docx = {};
};

class ILlmProvider {
public:
    virtual ~ILlmProvider() = default;
    virtual Result<ProposedCitation> generate(const Question& question,
                                               std::span<const Chunk> evidence) = 0;
};

}  // namespace rag
