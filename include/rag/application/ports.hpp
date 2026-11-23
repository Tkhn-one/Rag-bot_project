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
    void setPolicy(DocumentPolicy policy) noexcept { policy_ = policy; }
    const std::string& positionLayout() const noexcept { return positionLayout_; }

private:
    DocumentId id_;
    std::string source_;
    std::string content_;
    std::vector<Chunk> chunks_;
    DocumentPolicy policy_;
    std::string positionLayout_;
};

struct LibrarySnapshot {
    IndexRevision revision;
    DocumentPolicy policy;
    std::vector<IndexedDocument> documents;
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

    // Read metadata, policies and ordered documents at one revision.
    virtual Result<LibrarySnapshot> snapshot() const = 0;

    // Return true for a committed change, false for an already active policy.
    virtual Result<bool> setLibraryPolicy(DocumentPolicy policy) = 0;
    virtual Result<bool> setDocumentPolicy(DocumentId id, DocumentPolicy policy) = 0;
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
    bool fromCache = false;

    static ProposedCitation fromChunk(const Chunk& chunk, bool cached = false) {
        return {chunk.line(), chunk.text(), chunk.documentId(), chunk.page(), chunk.docx(), cached};
    }
};

struct GenerationRequest {
    const Question& question;
    const IndexRevision& revision;
    const ModelProfile& profile;
    std::span<const Chunk> evidence;
};

struct ChunkReference {
    DocumentId documentId;
    std::size_t line;
};

class IAnswerCacheStore {
public:
    virtual ~IAnswerCacheStore() = default;
    virtual Result<std::optional<ChunkReference>> lookup(const GenerationRequest& request) const = 0;
    virtual Result<bool> remember(const GenerationRequest& request, ChunkReference citation) = 0;
};

struct HistoryEntry {
    std::string timeUtc;
    std::string question;
    IndexRevision revision;
    ModelProfile profile;
    Answer answer;
    bool outdated;
};

class IHistoryStore {
public:
    virtual ~IHistoryStore() = default;
    virtual Result<bool> appendHistory(const std::string& question, const IndexRevision& revision,
                                       const ModelProfile& profile, const Answer& answer) = 0;
    virtual Result<std::vector<HistoryEntry>> history() const = 0;
    virtual Result<bool> clearHistory() = 0;
};

class ILlmProvider {
public:
    virtual ~ILlmProvider() = default;
    // Local metadata for the configured provider, stable throughout an ask operation.
    virtual ModelProfile profile() const = 0;
    virtual Result<ProposedCitation> generate(const GenerationRequest& request) = 0;
};

}  // namespace rag
