#include "rag/infrastructure/remote_api_provider.hpp"
#include "rag/infrastructure/utf8.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#include <winhttp.h>

#include <array>
#include <chrono>
#include <cstring>
#include <memory>

namespace rag {
namespace {

Result<std::string> asKey(std::wstring& password) {
    if (!password.empty() && password.back() == L'\0') password.pop_back();
    auto key = encodeUtf8(password);
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    if (!key) return AppError::ApiKeyUnavailable;
    if (key->empty()) return AppError::ApiKeyMissing;
    return std::move(*key);
}

Result<std::string> developmentKey() {
    constexpr auto name = L"OPENAI_API_KEY";
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return AppError::ApiKeyMissing;
    if (size > 4096) return AppError::ApiKeyUnavailable;
    std::wstring password(size, L'\0');
    const auto copied = GetEnvironmentVariableW(name, password.data(), size);
    if (copied >= size) return AppError::ApiKeyUnavailable;
    password.resize(copied);
    return asKey(password);
}

Result<std::string> credentialManagerKey() {
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(L"Rag-bot/OpenAI", CRED_TYPE_GENERIC, 0, &raw)) {
        return GetLastError() == ERROR_NOT_FOUND ? AppError::ApiKeyMissing : AppError::ApiKeyUnavailable;
    }
    const std::unique_ptr<CREDENTIALW, decltype(&CredFree)> credential(raw, CredFree);
    const auto bytes = credential->CredentialBlobSize;
    if (bytes == 0) return AppError::ApiKeyMissing;
    if (!credential->CredentialBlob || bytes % sizeof(wchar_t) != 0) return AppError::ApiKeyUnavailable;
    std::wstring password(bytes / sizeof(wchar_t), L'\0');
    std::memcpy(password.data(), credential->CredentialBlob, bytes);
    SecureZeroMemory(credential->CredentialBlob, bytes);
    return asKey(password);
}

using InternetHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

AppError networkError() {
    return GetLastError() == ERROR_WINHTTP_TIMEOUT ? AppError::RemoteTimeout : AppError::RemoteNetworkError;
}

bool configureConnection(HINTERNET session) {
    DWORD attempts = 1;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetTimeouts(session, 10000, 15000, 15000, 60000) ||
        !WinHttpSetOption(session, WINHTTP_OPTION_CONNECT_RETRIES, &attempts, sizeof(attempts)) ||
        !WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        return false;
    }
#ifdef WINHTTP_OPTION_FAILED_CONNECTION_RETRIES
    WINHTTP_FAILED_CONNECTION_RETRIES retries{};
    if (!WinHttpSetOption(session, WINHTTP_OPTION_FAILED_CONNECTION_RETRIES, &retries, sizeof(retries))) {
        // The option is absent from older WinHTTP versions. Each request uses a fresh connection.
        const auto error = GetLastError();
        return error == ERROR_WINHTTP_INVALID_OPTION || error == ERROR_NOT_SUPPORTED;
    }
#endif
    return true;
}

bool configureRequest(HINTERNET request) {
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_KEEP_ALIVE;
    return WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) &&
           WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
}

}  // namespace

Result<std::string> WindowsOpenAiSecrets::openAiKey() const {
    return source_ == ApiKeySource::DevelopmentEnvironment ? developmentKey() : credentialManagerKey();
}

Result<HttpResponse> WinHttpTransport::post(const HttpRequest& request) {
    if (request.url != RemoteApiProvider::endpoint || !std::in_range<DWORD>(request.jsonBody.size())) {
        return AppError::RemoteHttpError;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    const InternetHandle session(WinHttpOpen(L"Rag-bot/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
                                 WinHttpCloseHandle);
    if (!session || !configureConnection(session.get())) return networkError();
    const InternetHandle connection(WinHttpConnect(session.get(), L"api.openai.com", INTERNET_DEFAULT_HTTPS_PORT, 0),
                                    WinHttpCloseHandle);
    if (!connection) return networkError();
    const InternetHandle call(WinHttpOpenRequest(connection.get(), L"POST", L"/v1/responses", nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                              WinHttpCloseHandle);
    if (!call || !configureRequest(call.get())) return networkError();

    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer ";
    headers.append(request.bearerToken.begin(), request.bearerToken.end());
    const auto bytes = static_cast<DWORD>(request.jsonBody.size());
    const bool sent = WinHttpSendRequest(call.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                                        const_cast<char*>(request.jsonBody.data()), bytes, bytes, 0) != FALSE;
    const auto sendError = sent ? AppError::RemoteNetworkError : networkError();
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!sent) return sendError;
    if (!WinHttpReceiveResponse(call.get(), nullptr)) return networkError();

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(call.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        return networkError();
    }
    HttpResponse response{status, {}};
    if (status != 200) return response;

    constexpr std::size_t maxResponseBytes = 1024 * 1024;
    std::array<char, 8192> buffer;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD received = 0;
        if (!WinHttpReadData(call.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &received)) {
            return networkError();
        }
        if (received == 0) return response;
        if (received > maxResponseBytes - response.body.size()) return AppError::InvalidGeneration;
        response.body.append(buffer.data(), received);
    }
    return AppError::RemoteTimeout;
}

}  // namespace rag
