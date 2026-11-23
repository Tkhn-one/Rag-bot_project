#pragma once

#include "rag/application/ports.hpp"

#include <memory>

struct sqlite3;

namespace rag {

class SqliteKnowledgeStore final : public IKnowledgeStore, public IAnswerCacheStore, public IHistoryStore {
public:
    static Result<std::unique_ptr<SqliteKnowledgeStore>> open(const std::filesystem::path& path,
                                                              bool createIfMissing = true);
    static Result<std::unique_ptr<SqliteKnowledgeStore>> openInMemory();

    Result<std::size_t> save(std::string source, std::string content,
                             std::vector<DocumentLine> lines, DocumentPolicy policy,
                             std::string positionLayout = {}) override;
    Result<LibrarySnapshot> snapshot() const override;
    Result<bool> setLibraryPolicy(DocumentPolicy policy) override;
    Result<bool> setDocumentPolicy(DocumentId id, DocumentPolicy policy) override;
    Result<std::optional<ChunkReference>> lookup(const GenerationRequest& request) const override;
    Result<bool> remember(const GenerationRequest& request, ChunkReference citation) override;
    Result<bool> appendHistory(const std::string& question, const IndexRevision& revision,
                               const ModelProfile& profile, const Answer& answer) override;
    Result<std::vector<HistoryEntry>> history() const override;
    Result<bool> clearHistory() override;

private:
    struct CloseDatabase {
        void operator()(sqlite3* database) const noexcept;
    };
    using Database = std::unique_ptr<sqlite3, CloseDatabase>;

    explicit SqliteKnowledgeStore(Database database) : database_(std::move(database)) {}

    Database database_;
};

}  // namespace rag
