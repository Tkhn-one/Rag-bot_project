// Linked into the test CLI in place of windows_openai.cpp. Both counters are
// reported for every invocation, including early exits and history-only reads.
#include "rag/infrastructure/remote_api_provider.hpp"

#include <cstdio>

namespace {
struct Calls {
    int http = 0;
    int secrets = 0;
    ~Calls() { std::fprintf(stderr, "http_calls=%d key_reads=%d\n", http, secrets); }
} calls;
}

namespace rag {
Result<HttpResponse> WinHttpTransport::post(const HttpRequest&) {
    ++calls.http;
    return AppError::RemoteNetworkError;
}
Result<std::string> WindowsOpenAiSecrets::openAiKey() const {
    ++calls.secrets;
    return std::string("fixture");
}
}  // namespace rag
