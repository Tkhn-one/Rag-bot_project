#include "rag/application/services.hpp"
#include "rag/infrastructure/adapters.hpp"
#include "rag/infrastructure/local_embeddings.hpp"
#include "rag/infrastructure/local_llama_provider.hpp"
#include "rag/infrastructure/sqlite_knowledge_store.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace rag;
using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename T>
T value(Result<T> result) {
    if (const auto* error = std::get_if<AppError>(&result))
        throw std::runtime_error("AppError " + std::to_string(static_cast<int>(*error)));
    return std::get<T>(std::move(result));
}

class TimedSearch final : public IRetriever {
public:
    explicit TimedSearch(const IRetriever& inner) : inner_(inner) {}
    Result<std::vector<Chunk>> retrieve(const Question& q, const LibrarySnapshot& library) const override {
        const auto start = Clock::now();
        auto result = inner_.retrieve(q, library);
        const auto* chunks = std::get_if<std::vector<Chunk>>(&result);
        std::cout << "search_ms=" << elapsedMs(start) << " chunks=" << (chunks ? chunks->size() : 0) << '\n';
        require(chunks && !chunks->empty(), "demo must exercise a nonempty retrieval");
        return result;
    }
private:
    const IRetriever& inner_;
};

class ObservedGenerator final : public ILlmProvider {
public:
    explicit ObservedGenerator(ILlmProvider& inner) : inner_(inner) {}
    ModelProfile profile() const override { return inner_.profile(); }
    Result<std::size_t> evidenceCount(const GenerationRequest& request) const override {
        return inner_.evidenceCount(request);
    }
    Result<GenerationResult> generate(const GenerationRequest& request) override {
        ++calls;
        const auto start = Clock::now();
        auto result = inner_.generate(request);
        std::cout << "generation_ms=" << elapsedMs(start) << " sent=" << request.evidence.size() << '\n';
        if (damageQuote) {
            auto& generated = std::get<GenerationResult>(result);
            auto& answered = std::get<Answered>(generated.value);
            answered.claims.at(0).quote = "deliberately altered quotation";
        }
        return result;
    }
    int calls = 0;
    bool damageQuote = false;
private:
    ILlmProvider& inner_;
};

void run(const std::filesystem::path& demo, const std::filesystem::path& embeddingFile,
         const std::filesystem::path& generationFile) {
    auto store = value(SqliteKnowledgeStore::openInMemory());
    auto start = Clock::now();
    auto embeddings = value(openLocalEmbeddings(embeddingFile));
    std::cout << "embedding_load_ms=" << elapsedMs(start) << '\n';
    value(store->configureSearch(embeddings.get(), {}));
    TxtReader reader;
    IndexingService index(reader, *store);
    for (auto file : {"contract.txt", "decisions.txt"}) value(index.import(demo / file));

    start = Clock::now();
    auto generator = value(openLocalLlama(generationFile));
    std::cout << "generation_load_ms=" << elapsedMs(start) << '\n';
    const auto profile = generator->profile();
    const auto budgetQuestion = value(Question::fromText("Резервное копирование"));
    const IndexRevision revision{"budget-test", 1};
    const std::vector<Chunk> oversized = {{1, 1, "short"}, {1, 2, std::string(45000, '9')}};
    const GenerationRequest budget{budgetQuestion, revision, profile, oversized, {}};
    require(value(generator->evidenceCount(budget)) == 1, "context budget did not preserve the ranked prefix");
    const GenerationRequest tooLarge{budgetQuestion, revision, profile, budget.evidence.last(1), {}};
    require(generator->evidenceCount(tooLarge) == Result<std::size_t>(AppError::LocalContextTooLarge),
            "oversized first fragment accepted");
    // Qwen's tokenizer separates individual digits. This prompt uses the part
    // of the 8192-token window above 4096 while preserving the output reserve.
    const std::vector<Chunk> fits = {{1, 1, std::string(5000, '9')}};
    require(value(generator->evidenceCount({budgetQuestion, revision, profile, fits, {}})) == 1,
            "valid long prompt rejected");
    TimedSearch search(*store);
    ObservedGenerator observed(*generator);
    ValidatingLlmProvider validated(observed);
    CachingLlmProvider cached(validated, *store);
    RagService service(*store, search, cached, store.get());

    constexpr auto exact = "Резервное копирование";
    constexpr auto paraphrase = "В какой день делают страховочную копию данных проекта?";
    constexpr auto unsupported = "Какой пароль установлен для резервной копии данных проекта?";
    for (std::string_view question : {exact, paraphrase, unsupported, paraphrase}) {
        const int before = observed.calls;
        start = Clock::now();
        auto answer = value(service.ask(std::string(question)));
        std::cout << question << "\nask_ms=" << elapsedMs(start) << " cached=" << answer.fromCache()
                  << " generator_calls=" << observed.calls - before << '\n';
        if (answer.status() == AnswerStatus::InsufficientEvidence) std::cout << "insufficient_evidence\n";
        for (const auto& claim : answer.claims())
            std::cout << claim.text << "\n" << claim.citation.source() << ':' << claim.citation.line()
                      << " | " << claim.citation.quote() << '\n';
        if (question == unsupported) {
            require(answer.status() == AnswerStatus::InsufficientEvidence, "password has no supporting evidence");
        } else {
            const bool friday = std::ranges::any_of(answer.claims(), [](const CitedClaim& claim) {
                return claim.citation.source() == "decisions.txt" && claim.citation.line() == 4 &&
                       claim.citation.quote().find("пятниц") != std::string::npos &&
                       (claim.text.find("пятниц") != std::string::npos || claim.text.find("Пятниц") != std::string::npos);
            });
            require(friday, "expected Friday supported by decisions.txt line 4");
        }
        require(answer.fromCache() == (before == 3) && observed.calls == before + !answer.fromCache(),
                "unexpected cache hit or generation count");
    }
    require(value(store->history()).size() == 4, "successful questions must be recorded");

    // Reject a deliberately damaged model result before cache and history. Retry
    // the identical question through the unmodified generator at the same revision.
    constexpr auto retry = "Какой срок приёмки?";
    observed.damageQuote = true;
    const auto rejected = service.ask(retry);
    require(std::get_if<AppError>(&rejected) && std::get<AppError>(rejected) == AppError::InvalidCitation,
            "altered quote accepted");
    observed.damageQuote = false;
    require(!value(service.ask(retry)).fromCache(), "invalid citation was cached");

    // Do not keep two 4B models resident while testing an exhausted output budget.
    generator.reset();
    auto limited = value(openLocalLlama(generationFile, 1));
    ValidatingLlmProvider limitedValidated(*limited);
    CachingLlmProvider limitedCached(limitedValidated, *store);
    RagService limitedService(*store, search, limitedCached, store.get());
    for (int i = 0; i < 2; ++i) {
        const auto result = limitedService.ask(exact);
        require(std::get_if<AppError>(&result) && std::get<AppError>(result) == AppError::LocalGenerationFailed,
                "incomplete output was returned as a successful answer");
    }
    require(value(store->history()).size() == 5, "failed generation reached history");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4) return 2;
        run(argv[1], argv[2], argv[3]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
