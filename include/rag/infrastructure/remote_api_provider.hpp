#pragma once

#include "rag/application/ports.hpp"

namespace rag {

struct HttpRequest {
    std::string_view url;
    std::string_view bearerToken;
    std::string_view jsonBody;
};

struct HttpResponse {
    unsigned status;
    std::string body;
};

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    virtual Result<HttpResponse> post(const HttpRequest& request) = 0;
};

class ISecretStore {
public:
    virtual ~ISecretStore() = default;
    virtual Result<std::string> openAiKey() const = 0;
};

class RemoteApiProvider final : public ILlmProvider {
public:
    static constexpr std::string_view endpoint = "https://api.openai.com/v1/responses";
    static constexpr std::string_view defaultModel = "gpt-5.4-mini-2026-03-17";

    RemoteApiProvider(IHttpTransport& transport, const ISecretStore& secrets, std::string modelId,
                      std::string credentialScope);

    ModelProfile profile() const override { return profile_; }
    Result<std::size_t> evidenceCount(const GenerationRequest& request) const override;
    Result<GenerationResult> generate(const GenerationRequest& request) override;

private:
    IHttpTransport& transport_;
    const ISecretStore& secrets_;
    ModelProfile profile_;
};

enum class ApiKeySource { CredentialManager, DevelopmentEnvironment };

class WindowsOpenAiSecrets final : public ISecretStore {
public:
    explicit WindowsOpenAiSecrets(ApiKeySource source) : source_(source) {}
    Result<std::string> openAiKey() const override;

private:
    ApiKeySource source_;
};

class WinHttpTransport final : public IHttpTransport {
public:
    Result<HttpResponse> post(const HttpRequest& request) override;
};

}  // namespace rag
