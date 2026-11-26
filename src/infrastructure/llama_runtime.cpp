#include "llama_runtime.hpp"
#include "rag/infrastructure/local_path.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <array>

namespace rag {
namespace {
struct Backend {
    Backend() {
        llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
        llama_backend_init();
    }
    ~Backend() { llama_backend_free(); }
};


bool matchesSha256(HANDLE file, std::string_view expected) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)))
        return false;
    const std::unique_ptr<void, decltype(&BCryptDestroyHash)> owner(hash, BCryptDestroyHash);
    std::array<unsigned char, 65536> buffer;
    DWORD count;
    do {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) return false;
        if (count && !BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), count, 0))) return false;
    } while (count);
    std::array<unsigned char, 32> digest;
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), 32, 0))) return false;
    std::string hex;
    for (auto byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex == expected;
}
}  // namespace

Result<LlamaModel> loadVerifiedCpuModel(const std::filesystem::path& gguf, std::uintmax_t size,
                                       std::string_view sha256) {
    std::error_code ec;
    const auto path = std::filesystem::absolute(gguf, ec).lexically_normal();
    if (ec || !isLocalStoragePath(path)) return AppError::LocalModelUnavailable;
    // Deny replacement or modification between verification and llama.cpp opening the same file.
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return AppError::LocalModelUnavailable;
    const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, CloseHandle);
    LARGE_INTEGER length;
    if (!GetFileSizeEx(handle, &length)) return AppError::LocalModelUnavailable;
    if (static_cast<std::uintmax_t>(length.QuadPart) != size || !matchesSha256(handle, sha256))
        return AppError::LocalModelMismatch;
    static Backend backend;
    const auto filename = path.u8string();
    auto options = llama_model_default_params();
    options.n_gpu_layers = 0;
    LlamaModel model(llama_model_load_from_file(reinterpret_cast<const char*>(filename.c_str()), options),
                     llama_model_free);
    if (!model) return AppError::LocalModelLoadFailed;
    return model;
}
}  // namespace rag
