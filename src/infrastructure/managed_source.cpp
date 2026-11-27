#include "managed_source.hpp"
#include "cache_key.hpp"
#include "local_file.hpp"
#include "rag/infrastructure/local_path.hpp"
#include "rag/infrastructure/utf8.hpp"
#include <sqlite3.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace rag {
namespace fs = std::filesystem;
namespace {
constexpr std::size_t maxBinarySize = 64 * 1024 * 1024;

std::string hexDigest(std::string_view bytes) {
    const auto digest = sha256(bytes);
    if (!digest) return {};
    std::string hex;
    constexpr char alphabet[] = "0123456789abcdef";
    for (auto byte : *digest) {
        hex += alphabet[byte >> 4];
        hex += alphabet[byte & 15];
    }
    return hex;
}
}

bool validDigest(std::string_view digest) {
    return digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == digest.npos;
}

fs::path sourceDirectory(sqlite3* db, const std::string& libraryId) {
    const char* filename = sqlite3_db_filename(db, "main");
    if (!filename || !*filename || libraryId.size() != 32 ||
        libraryId.find_first_not_of("0123456789abcdef") != std::string::npos) return {};
    const auto path = decodeUtf8(filename);
    if (!path) return {};
    return fs::path(*path).parent_path() / "sources" / libraryId;
}

fs::path sourceExtension(const fs::path& filename) {
    for (auto ext : {L".txt", L".pdf", L".docx"}) {
        if (hasExtension(filename, ext)) return ext;
    }
    return {};
}

bool verifiedCopy(const fs::path& path, std::string_view expected) {
    if (!validDigest(expected) || !isLocalStoragePath(path)) return false;
    const auto bytes = readLocalFile(path, maxBinarySize);
    const auto* data = std::get_if<std::string>(&bytes);
    return data && hexDigest(*data) == expected;
}

void StagedSource::unlock() {
    if (readLock_) CloseHandle(readLock_);
    readLock_ = nullptr;
}

StagedSource::~StagedSource() {
    unlock();
    if (!path_.empty()) DeleteFileW(path_.c_str());
}

Result<std::string> StagedSource::copy(const fs::path& input, const fs::path& directory) {
    const auto ext = sourceExtension(input);
    if (ext.empty()) return AppError::UnsupportedFormat;
    if (!isLocalStoragePath(input)) return AppError::FileUnavailable;
    const auto bytes = readLocalFile(input, ext == L".txt" ? 8 * 1024 * 1024 : maxBinarySize);
    if (auto* error = std::get_if<AppError>(&bytes)) return *error;
    const auto& data = std::get<std::string>(bytes);
    const auto digest = hexDigest(data);
    if (digest.empty() || directory.empty()) return AppError::StoreWriteFailed;

    std::error_code ec;
    for (const auto& folder : {directory.parent_path(), directory}) {
        if (!isLocalStoragePath(folder, true)) return AppError::StoreWriteFailed;
        fs::create_directory(folder, ec);
        if (ec || !isLocalStoragePath(folder)) return AppError::StoreWriteFailed;
    }
    std::array<unsigned char, 16> random{};
    sqlite3_randomness(static_cast<int>(random.size()), random.data());
    const auto nonce = hexDigest({reinterpret_cast<const char*>(random.data()), random.size()});
    if (nonce.empty()) return AppError::StoreWriteFailed;
    auto temporary = directory / ("partial-" + nonce);
    temporary += ext;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return AppError::StoreWriteFailed;
    path_ = std::move(temporary);
    DWORD written = 0;
    const bool complete = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
                          && written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!complete) return AppError::StoreWriteFailed;
    const auto lock = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return AppError::StoreWriteFailed;
    readLock_ = lock;
    if (!verifiedCopy(path_, digest)) return AppError::StoreWriteFailed;
    return digest;
}

bool StagedSource::publish(const fs::path& target) {
    unlock();
    if (!isLocalStoragePath(target, true) ||
        !MoveFileExW(path_.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    path_.clear();
    return true;
}
} // namespace rag
