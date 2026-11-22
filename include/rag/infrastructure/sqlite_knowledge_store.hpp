#pragma once

#include "rag/application/ports.hpp"

#include <memory>

struct sqlite3;

namespace rag {

class SqliteKnowledgeStore final : public IKnowledgeStore {
public:
    static Result<std::unique_ptr<SqliteKnowledgeStore>> open(const std::filesystem::path& path);

    Result<std::size_t> save(std::string source, std::string content,
                             std::vector<DocumentLine> lines, DocumentPolicy policy,
                             std::string positionLayout = {}) override;
    Result<LibrarySnapshot> snapshot() const override;
    Result<bool> setLibraryPolicy(DocumentPolicy policy) override;
    Result<bool> setDocumentPolicy(DocumentId id, DocumentPolicy policy) override;

private:
    struct CloseDatabase {
        void operator()(sqlite3* database) const noexcept;
    };
    using Database = std::unique_ptr<sqlite3, CloseDatabase>;

    explicit SqliteKnowledgeStore(Database database) : database_(std::move(database)) {}

    Database database_;
};

}  // namespace rag
