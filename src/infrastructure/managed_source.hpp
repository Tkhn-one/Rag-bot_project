#pragma once

#include "rag/domain/model.hpp"
#include <filesystem>

struct sqlite3;

namespace rag {

std::filesystem::path sourceDirectory(sqlite3* db, const std::string& libraryId);
std::filesystem::path sourceExtension(const std::filesystem::path& filename);
bool validDigest(std::string_view digest);
bool verifiedCopy(const std::filesystem::path& path, std::string_view digest);

// Owned staging file. The caller holds the library's SQLite write transaction
// until publication. Destroying a failed import releases its private copy.
class StagedSource {
public:
    StagedSource() = default;
    StagedSource(const StagedSource&) = delete;
    StagedSource& operator=(const StagedSource&) = delete;
    ~StagedSource();
    Result<std::string> copy(const std::filesystem::path& input, const std::filesystem::path& directory);
    bool publish(const std::filesystem::path& target);
    const std::filesystem::path& path() const { return path_; }
private:
    void unlock();
    void* readLock_ = nullptr; // Windows handle denying writes and renames while the parser reads.
    std::filesystem::path path_;
};

} // namespace rag
