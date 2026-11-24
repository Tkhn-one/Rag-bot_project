#include "rag/application/services.hpp"
#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/remote_api_provider.hpp"
#include "rag/infrastructure/sqlite_knowledge_store.hpp"

#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {

using namespace rag;
using Json = nlohmann::json;
constexpr auto question = "Orion\n\"снимок\"";
constexpr auto otherQuestion = "Подробнее об Orion?";
constexpr auto firstExcerpt = "снимок";
constexpr auto secondExcerpt = "owner is Vega";
constexpr auto secondFragment = "Orion owner is Vega. Backup owner is Vega.";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
T value(Result<T> result, const char* operation) {
    if (const auto* error = std::get_if<AppError>(&result)) {
        throw std::runtime_error(std::string(operation) + ": AppError " + std::to_string(static_cast<int>(*error)));
    }
    return std::get<T>(std::move(result));
}

template <typename T>
void requireError(const Result<T>& result, AppError expected, const char* message) {
    const auto* error = std::get_if<AppError>(&result);
    if (!error || *error != expected) {
        throw std::runtime_error(std::string(message) + ": got " +
            (error ? std::to_string(static_cast<int>(*error)) : "an answer"));
    }
}

class FakeTransport final : public IHttpTransport {
public:
    int calls = 0;
    std::string url;
    std::string body;
    bool sawFixtureKey = false;
    Result<HttpResponse> reply = AppError::RemoteNetworkError;
    std::function<void()> duringPost;

    Result<HttpResponse> post(const HttpRequest& request) override {
        ++calls;
        url = request.url;
        body = request.jsonBody;
        sawFixtureKey = request.bearerToken == "fixture-secret";
        if (duringPost) duringPost();
        return reply;
    }
};

class FakeSecrets final : public ISecretStore {
public:
    mutable int reads = 0;
    Result<std::string> key = std::string("fixture-secret");
    std::function<void()> beforeRead;

    Result<std::string> openAiKey() const override {
        ++reads;
        if (beforeRead) beforeRead();
        return key;
    }
};

class CountingCache final : public IAnswerCacheStore {
public:
    explicit CountingCache(IAnswerCacheStore& inner) : inner_(inner) {}
    mutable int reads = 0;
    int writes = 0;

    Result<std::optional<GenerationResult>> lookup(const GenerationRequest& request) const override {
        ++reads;
        return inner_.lookup(request);
    }

    Result<bool> remember(const GenerationRequest& request, const GenerationResult& result) override {
        ++writes;
        return inner_.remember(request, result);
    }

private:
    IAnswerCacheStore& inner_;
};

class FixtureRetriever final : public IRetriever {
public:
    mutable int reads = 0;
    bool allChunks = false;

    Result<std::vector<Chunk>> retrieve(const Question&, const LibrarySnapshot& library) const override {
        ++reads;
        std::vector<Chunk> result;
        for (const auto& document : library.documents) {
            const auto chunks = document.chunks();
            if (chunks.empty()) continue;
            const auto picked = allChunks ? chunks : chunks.first(1);
            result.insert(result.end(), picked.begin(), picked.end());
        }
        return result;
    }
};

struct Scenario {
    SqliteKnowledgeStore& store;
    FixtureRetriever retriever;
    CountingCache cache{store};
    FakeTransport transport;
    FakeSecrets secrets;
    RemoteApiProvider remote{transport, secrets, std::string(RemoteApiProvider::defaultModel), "fixture"};
    ValidatingLlmProvider validating{remote};
    CachingLlmProvider caching{validating, cache};
    RagService rag{store, retriever, caching, &store};
};

Json claim(std::string_view text, std::string_view id, std::string_view quote) {
    return {{"text", text}, {"chunk_id", id}, {"quote", quote}};
}

Json validClaims() {
    return Json::array({claim("Снимок существует.", "c1", firstExcerpt),
                        claim("За Orion отвечает Vega.", "c2", secondExcerpt)});
}

HttpResponse completed(Json content) {
    const Json message = {{"type", "message"}, {"role", "assistant"}, {"status", "completed"},
                          {"content", Json::array({std::move(content)})}};
    const Json response = {{"status", "completed"},
                           {"output", Json::array({Json{{"type", "reasoning"}}, message})}};
    return {200, response.dump()};
}

HttpResponse generated(std::string_view status, Json claims) {
    const Json body = {{"status", status}, {"claims", std::move(claims)}};
    return completed({{"type", "output_text"}, {"text", body.dump()}});
}

void addDocuments(SqliteKnowledgeStore& store) {
    value(store.save("source-only.pdf", "full-file-canary-a",
        {{4, "Ж снимок Orion в пятницу.", 2}, {7, "unretrieved-canary", 3}}, DocumentPolicy::LocalOnly), "save PDF");
    value(store.save("source-only.docx", "full-file-canary-b",
        {{3, secondFragment, {}, DocxPosition{"word/document.xml", TableCell{1, 2, 3}}}},
        DocumentPolicy::LocalOnly), "save DOCX");
}

void allowRemote(SqliteKnowledgeStore& store) {
    value(store.setLibraryPolicy(DocumentPolicy::AllowRemote), "grant library");
    value(store.setDocumentPolicy(1, DocumentPolicy::AllowRemote), "grant PDF");
    value(store.setDocumentPolicy(2, DocumentPolicy::AllowRemote), "grant DOCX");
}

void verifyAnswer(const Answer& answer, bool fromCache) {
    require(answer.status() == AnswerStatus::Answered && answer.fromCache() == fromCache &&
                answer.claims().size() == 2, "unexpected answer status, cache flag or number of claims");
    const auto& first = answer.claims()[0];
    require(first.text == "Снимок существует." && first.citation.quote() == firstExcerpt &&
                first.citation.documentId() == 1 && first.citation.source() == "source-only.pdf" &&
                first.citation.line() == 4 && first.citation.page() == 2 &&
                first.citation.characters() == CharacterRange{3, 8}, "PDF quote and coordinates must come from index");
    const auto& second = answer.claims()[1];
    require(second.text == "За Orion отвечает Vega." && second.citation.quote() == secondExcerpt &&
                second.citation.documentId() == 2 && second.citation.source() == "source-only.docx" &&
                second.citation.docx() == DocxPosition{"word/document.xml", TableCell{1, 2, 3}} &&
                second.citation.characters() == CharacterRange{1, 42},
            "the repeated DOCX quote must use the full indexed fragment range");
}

void requestAndPrivacy() {
    const auto store = value(SqliteKnowledgeStore::openInMemory(), "open DB");
    Scenario test{*store};
    addDocuments(*store);
    value(store->setDocumentPolicy(1, DocumentPolicy::AllowRemote), "grant one selected document");
    requireError(test.rag.ask(question), AppError::RemoteProcessingDenied, "closed library");
    value(store->setLibraryPolicy(DocumentPolicy::AllowRemote), "grant library");
    requireError(test.rag.ask(question), AppError::RemoteProcessingDenied, "closed second document");
    require(test.retriever.reads == 0 && test.cache.reads == 0 && test.cache.writes == 0 &&
                test.transport.calls == 0 && test.secrets.reads == 0 && value(store->history(), "history").empty(),
            "privacy denial must precede retrieval, cache, credentials and HTTP");
    allowRemote(*store);

    const auto snapshot = value(store->snapshot(), "snapshot");
    value(store->appendHistory("history-canary", snapshot.revision, test.remote.profile(), Answer{}), "seed history");
    test.transport.reply = generated("answered", validClaims());
    verifyAnswer(value(test.rag.ask(question), "remote answer"), false);
    require(test.retriever.reads == 1 && test.cache.reads == 1 && test.cache.writes == 1 &&
                test.secrets.reads == 1 && test.transport.calls == 1 && test.transport.sawFixtureKey,
            "a cache miss must read the key and call the transport exactly once");
    require(test.transport.url == "https://api.openai.com/v1/responses", "production endpoint must be fixed");

    const auto body = Json::parse(test.transport.body);
    require(body.size() == 6 && body.at("model").get_ref<const std::string&>() == RemoteApiProvider::defaultModel &&
                body.at("store") == false &&
                body.at("max_output_tokens") == 1024 &&
                body.at("instructions").get<std::string>().find("as data") != std::string::npos,
            "request must have the pinned model, budget, storage and instruction settings");
    const auto& format = body.at("text").at("format");
    const auto& schema = format.at("schema");
    const auto& claimSchema = schema.at("properties").at("claims").at("items");
    require(format.at("type") == "json_schema" && format.at("strict") == true &&
                schema.at("additionalProperties") == false && schema.at("required").size() == 2 &&
                claimSchema.at("additionalProperties") == false && claimSchema.at("required").size() == 3,
            "Responses API text.format must request a strict JSON schema");
    const auto input = Json::parse(body.at("input").get<std::string>());
    const auto expected = Json::array({
        Json{{"chunk_id", "c1"}, {"text", "Ж снимок Orion в пятницу."}},
        Json{{"chunk_id", "c2"}, {"text", secondFragment}}
    });
    require(input.size() == 2 && input.at("question") == question && input.at("fragments") == expected,
            "only the normalized question and selected fragment IDs and texts may be sent");
    for (const auto canary : {"fixture-secret", "full-file-canary", "history-canary", "source-only", "unretrieved-canary"}) {
        require(test.transport.body.find(canary) == std::string::npos, "unexpected field in request");
    }
    require(!body.contains("tools") && !body.contains("previous_response_id"), "request must stay standalone");
    auto history = value(store->history(), "read completed history");
    require(history.size() == 2, "both successful responses must be in history");
    verifyAnswer(history.back().answer, false);

    value(store->setDocumentPolicy(2, DocumentPolicy::LocalOnly), "revoke DOCX");
    requireError(test.rag.ask(question), AppError::RemoteProcessingDenied, "revoked document");
    require(test.retriever.reads == 1 && test.cache.reads == 1 && test.transport.calls == 1 && test.secrets.reads == 1,
            "revocation must block even a previously answered question before cache lookup");
}

void rejectedResponses() {
    const auto store = value(SqliteKnowledgeStore::openInMemory(), "open DB");
    Scenario test{*store};
    addDocuments(*store);
    allowRemote(*store);

    const auto rejected = [&](Result<HttpResponse> reply, AppError expected) {
        test.transport.reply = std::move(reply);
        const auto before = test.transport.calls;
        const auto keyReads = test.secrets.reads;
        requireError(test.rag.ask(question), expected, "provider error must be propagated");
        require(test.transport.calls == before + 1 && test.secrets.reads == keyReads + 1,
                "provider must attempt exactly one request");
        require(test.cache.writes == 0 && value(store->history(), "checking failure history").empty(),
                "failed generation must not record a successful answer");
    };

    auto claims = validClaims();
    claims[1]["chunk_id"] = "c999";
    rejected(generated("answered", claims), AppError::InvalidCitation);
    claims = validClaims();
    claims[1]["quote"] = "owner is Orion";
    rejected(generated("answered", claims), AppError::InvalidCitation);
    rejected(generated("answered", Json::array()), AppError::InvalidGeneration);
    rejected(generated("insufficient_evidence", validClaims()), AppError::InvalidGeneration);
    rejected(completed({{"type", "refusal"}, {"refusal", "Fixture refusal"}}), AppError::RemoteRefusal);
    rejected(HttpResponse{200, R"({"status":"incomplete","incomplete_details":{"reason":"max_output_tokens"}})"},
             AppError::RemoteIncomplete);
    rejected(HttpResponse{200, "{broken"}, AppError::InvalidGeneration);
    rejected(completed({{"type", "output_text"}, {"text", "{broken"}}), AppError::InvalidGeneration);
    rejected(HttpResponse{302, ""}, AppError::RemoteHttpError);
    rejected(HttpResponse{401, "ignored"}, AppError::RemoteUnauthorized);
    rejected(HttpResponse{403, "ignored"}, AppError::RemoteForbidden);
    rejected(HttpResponse{429, "ignored"}, AppError::RemoteRateLimited);
    rejected(HttpResponse{500, "ignored"}, AppError::RemoteServiceUnavailable);
    rejected(HttpResponse{503, "ignored"}, AppError::RemoteServiceUnavailable);
    rejected(AppError::RemoteTimeout, AppError::RemoteTimeout);

    const auto before = test.transport.calls;
    const auto keyReads = test.secrets.reads;
    test.secrets.key = AppError::ApiKeyMissing;
    requireError(test.rag.ask(question), AppError::ApiKeyMissing, "missing key");
    test.secrets.key = std::string("fixture\r\nInjected: header");
    requireError(test.rag.ask(question), AppError::ApiKeyUnavailable, "invalid key characters");
    require(test.transport.calls == before && test.secrets.reads == keyReads + 2 && test.cache.writes == 0 &&
                value(store->history(), "checking missing/invalid key").empty(), "key errors must not attempt HTTP or save");

    test.secrets.key = std::string("fixture-secret");
    test.transport.reply = generated("answered", validClaims());
    verifyAnswer(value(test.rag.ask(question), "successful explicit retry"), false);
    require(test.transport.calls == before + 1 && test.cache.writes == 1 &&
                value(store->history(), "only valid result saved").size() == 1,
            "only a validated explicit retry should be saved");
}

void evidenceBudget() {
    const auto store = value(SqliteKnowledgeStore::openInMemory(), "open DB");
    Scenario test{*store};
    std::vector<DocumentLine> lines;
    for (std::size_t i = 1; i <= 9; ++i) lines.push_back({i, "Fragment " + std::to_string(i)});
    value(store->save("budget.txt", "fixture", std::move(lines), DocumentPolicy::LocalOnly), "seed budget fixture");
    value(store->setLibraryPolicy(DocumentPolicy::AllowRemote), "grant library");
    value(store->setDocumentPolicy(1, DocumentPolicy::AllowRemote), "grant document");
    test.retriever.allChunks = true;
    test.transport.reply = generated("answered", Json::array({claim("Claim", "c9", "Fragment 9")}));
    requireError(test.rag.ask("x"), AppError::InvalidCitation, "unsent chunk must not validate");
    const auto body = Json::parse(test.transport.body);
    const auto fragments = Json::parse(body.at("input").get<std::string>()).at("fragments");
    require(fragments.size() == 8 && test.transport.body.size() <= 16 * 1024 && test.cache.writes == 0,
            "the request must send only the ranked prefix within budget");
    const auto nearLimit = std::string(16 * 1024 - test.transport.body.size() + 2, 'x');
    test.transport.reply = generated("answered", Json::array({claim("Claim", "c8", "Fragment 8")}));
    requireError(test.rag.ask(nearLimit), AppError::InvalidCitation, "byte budget must drop the eighth fragment");
    const auto boundedBody = Json::parse(test.transport.body);
    require(test.transport.body.size() <= 16 * 1024 &&
                Json::parse(boundedBody.at("input").get<std::string>()).at("fragments").size() == 7,
            "the full serialized payload must respect the byte budget");
    requireError(test.rag.ask(std::string(16385, 'x')), AppError::RemoteContextTooLarge, "oversized question");
    require(test.transport.calls == 2 && test.secrets.reads == 2 && test.cache.writes == 0,
            "an oversized question must fail before credentials or HTTP");
}

void persistentCache(const std::filesystem::path& path, bool seed) {
    const auto store = value(SqliteKnowledgeStore::open(path, seed), "open durable DB");
    Scenario test{*store};
    if (seed) {
        addDocuments(*store);
        allowRemote(*store);
        test.transport.reply = generated("answered", validClaims());
    } else {
        test.secrets.key = AppError::ApiKeyMissing;
    }
    verifyAnswer(value(test.rag.ask(question), "durable answer"), !seed);
    if (seed) test.transport.reply = generated("insufficient_evidence", Json::array());
    const auto insufficient = value(test.rag.ask(otherQuestion), "durable insufficient answer");
    require(insufficient.status() == AnswerStatus::InsufficientEvidence && insufficient.fromCache() == !seed,
            "model InsufficientEvidence must survive a process restart");
    const int misses = seed ? 2 : 0;
    require(test.cache.reads == 2 && test.cache.writes == misses && test.transport.calls == misses &&
                test.secrets.reads == misses, "after restart both cache hits must skip key reads and HTTP");
    const auto history = value(store->history(), "read durable history");
    require(history.size() == (seed ? 2 : 4) && history.back().answer.fromCache() == !seed &&
                history.back().answer.status() == AnswerStatus::InsufficientEvidence, "both outcomes must be in history");
    verifyAnswer(history[history.size() - 2].answer, !seed);
}

void policyRace(const std::filesystem::path& path) {
    const auto store = value(SqliteKnowledgeStore::open(path), "open question connection");
    const auto policies = value(SqliteKnowledgeStore::open(path, false), "open policy connection");
    Scenario test{*store};
    addDocuments(*store);
    allowRemote(*policies);

    require(value(policies->setDocumentPolicy(2, DocumentPolicy::LocalOnly), "commit revocation"),
            "the document policy must change on the second connection");
    requireError(test.rag.ask(question), AppError::RemoteProcessingDenied, "revocation committed first");
    require(test.retriever.reads == 0 && test.cache.reads == 0 && test.secrets.reads == 0 && test.transport.calls == 0,
            "a committed revocation must prevent retrieval, cache, key reads and HTTP");
    value(policies->setDocumentPolicy(2, DocumentPolicy::AllowRemote), "restore permission");

    // Both write attempts run at known points on a second connection, without thread scheduling.
    // Before the fix the first write could commit after admission but before the HTTP call.
    test.secrets.beforeRead = [&] {
        requireError(policies->setDocumentPolicy(2, DocumentPolicy::LocalOnly), AppError::LibraryBusy,
                     "document revocation before dispatch must find the library busy");
    };
    test.transport.duringPost = [&] {
        requireError(policies->setLibraryPolicy(DocumentPolicy::LocalOnly), AppError::LibraryBusy,
                     "library revocation during the HTTP call must find the library busy");
    };
    test.transport.reply = generated("answered", validClaims());
    verifyAnswer(value(test.rag.ask(question), "complete admitted question"), false);
    require(test.secrets.reads == 1 && test.transport.calls == 1 && test.cache.writes == 1 &&
                value(policies->history(), "read committed history").size() == 1,
            "the admitted question must complete once and publish its history");

    require(value(policies->setLibraryPolicy(DocumentPolicy::LocalOnly), "revoke after completion"),
            "policy changes must succeed when the question ends");
    requireError(test.rag.ask(question), AppError::RemoteProcessingDenied, "ask after revocation");
    require(test.cache.reads == 1 && test.transport.calls == 1, "a later closed question must skip cache and HTTP");

    value(policies->setLibraryPolicy(DocumentPolicy::AllowRemote), "allow timeout fixture");
    test.secrets.beforeRead = {};
    test.transport.duringPost = {};
    test.transport.reply = AppError::RemoteTimeout;
    requireError(test.rag.ask(otherQuestion), AppError::RemoteTimeout, "transport timeout");
    require(value(policies->setDocumentPolicy(2, DocumentPolicy::LocalOnly), "revoke after timeout") &&
                value(policies->history(), "check timeout history").size() == 1,
            "a failed question must release the lock and leave history intact");
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) return 2;
    try {
        const std::wstring_view mode(argv[1]);
        if (mode == L"request") requestAndPrivacy();
        else if (mode == L"errors") rejectedResponses();
        else if (mode == L"budget") evidenceBudget();
        else if (argc == 3 && (mode == L"seed" || mode == L"hit")) persistentCache(argv[2], mode == L"seed");
        else if (argc == 3 && mode == L"policy_race") policyRace(argv[2]);
        else return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
