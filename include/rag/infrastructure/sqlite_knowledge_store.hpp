#pragma once

#include "rag/application/ports.hpp"

#include <memory>

struct sqlite3;

namespace rag {

class SqliteKnowledgeStore final : public IKnowledgeStore, public IAnswerCacheStore, public IHistoryStore, public IRetriever, public ICitationSource {
public:
    static Result<std::unique_ptr<SqliteKnowledgeStore>> open(const std::filesystem::path& path,
                                                              bool createIfMissing = true);
    static Result<std::unique_ptr<SqliteKnowledgeStore>> openInMemory();

    Result<ImportReport> importSource(const std::filesystem::path& path, const ParseSource& parse) override;
    Result<CitationSource> resolveSource(const IndexRevision& revision, const Citation& citation) const override;
    Result<std::size_t> save(std::string source, std::string content,
                             std::vector<DocumentLine> lines, DocumentPolicy policy,
                             std::string positionLayout = {}) override;
    Result<LibrarySnapshot> snapshot() const override;
    Result<Answer> runRemoteQuestion(const RemoteQuestion& question) const override;
    Result<bool> setLibraryPolicy(DocumentPolicy policy) override;
    Result<bool> setDocumentPolicy(DocumentId id, DocumentPolicy policy) override;
    Result<std::optional<GenerationResult>> lookup(const GenerationRequest& request) const override;
    Result<bool> remember(const GenerationRequest& request, const GenerationResult& result) override;
    Result<bool> appendHistory(const std::string& question, const IndexRevision& revision,
                               const ModelProfile& profile, const Answer& answer) override;
    Result<std::vector<HistoryEntry>> history() const override;
    Result<bool> clearHistory() override;
    // Provider is local and must outlive subsequent indexing and search operations.
    Result<bool> configureSearch(const IEmbeddingProvider* provider, SearchSettings settings = {}, bool rebuild = false);
    Result<std::vector<Chunk>> retrieve(const Question& question, const LibrarySnapshot& library) const override;

private:
    struct CloseDatabase {
        void operator()(sqlite3* database) const noexcept;
    };
    using Database = std::unique_ptr<sqlite3, CloseDatabase>;

    explicit SqliteKnowledgeStore(Database database) : database_(std::move(database)) {}

    Database database_;
    const IEmbeddingProvider* embeddings_ = nullptr;
    SearchSettings settings_;
};

}  // namespace rag
