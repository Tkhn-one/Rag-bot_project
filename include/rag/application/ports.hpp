#pragma once

#include "rag/domain/model.hpp"

#include <charconv>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
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

struct ImportReport {
    std::size_t chunks;
    std::vector<std::size_t> pagesWithoutText;
    bool incomplete;
};

struct ParsedSource {
    std::string content;
    std::string layout;
    std::vector<DocumentLine> lines;
    ImportReport report;
};

enum class CopyStatus { Available, Legacy, Unavailable };

struct CitationSource {
    std::string blockText;
    CharacterRange highlight;
    CopyStatus copyStatus;
    std::filesystem::path copyPath;
    std::string sha256;
};

class ICitationSource {
public:
    virtual ~ICitationSource() = default;
    virtual Result<CitationSource> resolveSource(const IndexRevision& revision, const Citation& citation) const = 0;
};

class IKnowledgeStore {
public:
    using RemoteQuestion = std::function<Result<Answer>()>;

    virtual ~IKnowledgeStore() = default;

    using ParseSource = std::function<Result<ParsedSource>(const std::filesystem::path&)>;
    virtual Result<ImportReport> importSource(const std::filesystem::path& path, const ParseSource& parse);

    // Publish the document and its lines together. Line numbers must be positive and unique.
    // Position layout is empty for TXT. For equal text and layout, return the existing chunk count.
    virtual Result<std::size_t> save(std::string source, std::string content,
                                     std::vector<DocumentLine> lines, DocumentPolicy policy,
                                     std::string positionLayout = {}) = 0;

    // Read metadata, policies and ordered documents at one revision.
    virtual Result<LibrarySnapshot> snapshot() const = 0;

    // Run synchronously under the same exclusion used for policy writes, starting before the snapshot.
    virtual Result<Answer> runRemoteQuestion(const RemoteQuestion& question) const = 0;

    // Return true for a committed change, false for an already active policy.
    virtual Result<bool> setLibraryPolicy(DocumentPolicy policy) = 0;
    virtual Result<bool> setDocumentPolicy(DocumentId id, DocumentPolicy policy) = 0;
};

enum class EmbeddingInput { Document, Query };

class IEmbeddingProvider {
public:
    virtual ~IEmbeddingProvider() = default;
    virtual std::string profileId() const = 0;
    virtual std::size_t dimension() const = 0;
    // Token budget for embedding a document slice, including any model delimiters.
    virtual Result<std::size_t> documentTokenCount(std::string_view text) const = 0;
    virtual Result<std::vector<float>> embed(std::string_view text, EmbeddingInput input) const = 0;
};

struct SearchSettings {
    int candidates = 20;
    int rrfConstant = 60;
    int contextLimit = 8;
    int perDocument = 2;
};

class IRetriever {
public:
    virtual ~IRetriever() = default;
    virtual Result<std::vector<Chunk>> retrieve(const Question& question, const LibrarySnapshot& library) const = 0;
};

struct ProposedClaim {
    std::string text;
    std::string chunkId;
    std::string quote;
};

struct Answered {
    std::vector<ProposedClaim> claims;
};

struct InsufficientEvidence {};

struct GenerationResult {
    std::variant<InsufficientEvidence, Answered> value;
    bool fromCache = false;
};

// IDs belong to one ordered request. They reveal no source metadata.
inline std::string contextChunkId(std::size_t index) {
    return "c" + std::to_string(index + 1);
}

inline std::optional<std::size_t> contextChunkIndex(std::string_view id, std::size_t evidenceSize) {
    if (id.size() < 2 || id.front() != 'c' || id[1] == '0') {
        return std::nullopt;
    }
    std::size_t number = 0;
    const auto end = id.data() + id.size();
    const auto parsed = std::from_chars(id.data() + 1, end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end || number == 0 || number > evidenceSize) {
        return std::nullopt;
    }
    return number - 1;
}

struct GenerationRequest {
    const Question& question;
    const IndexRevision& revision;
    const ModelProfile& profile;
    std::span<const Chunk> evidence;
    std::span<const IndexedDocument> documents;
};

class IAnswerCacheStore {
public:
    virtual ~IAnswerCacheStore() = default;
    virtual Result<std::optional<GenerationResult>> lookup(const GenerationRequest& request) const = 0;
    virtual Result<bool> remember(const GenerationRequest& request, const GenerationResult& result) = 0;
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
    // A provider may use a prefix of the ranked context to fit its input budget.
    virtual Result<std::size_t> evidenceCount(const GenerationRequest& request) const {
        return request.evidence.size();
    }
    virtual Result<GenerationResult> generate(const GenerationRequest& request) = 0;
};

}  // namespace rag
