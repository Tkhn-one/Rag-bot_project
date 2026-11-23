#include "cache_key.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <string_view>

namespace rag {

std::optional<CacheDigest> cacheKey(const GenerationRequest& request) {
    std::string bytes;
    const auto field = [&](std::string_view value) {
        bytes += std::to_string(value.size());
        bytes += ':';
        bytes += value;
    };
    field(request.question.text());
    field(request.revision.libraryId);
    field(std::to_string(request.revision.number));
    const auto& profile = request.profile;
    field(profile.processing == ProcessingType::Local ? "local" : "remote");
    field(profile.endpoint);
    field(profile.modelId);
    field(profile.generationParameters);
    field(profile.responseFormat);
    field(profile.credentialScope);

    CacheDigest digest;
    if (!std::in_range<ULONG>(bytes.size()) ||
        !BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
            digest.data(), static_cast<ULONG>(digest.size())))) {
        return std::nullopt;
    }
    return digest;
}

}  // namespace rag
