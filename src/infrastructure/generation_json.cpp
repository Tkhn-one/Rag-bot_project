#include "generation_json.hpp"

namespace rag {
using Json = nlohmann::json;

std::string generationInput(const GenerationRequest& request, std::size_t count) {
    auto fragments = Json::array();
    for (std::size_t i = 0; i < count; ++i) {
        fragments.push_back({{"chunk_id", contextChunkId(i)}, {"text", request.evidence[i].text()}});
    }
    const Json input = {{"question", request.question.text()}, {"fragments", std::move(fragments)}};
    return input.dump();
}

const Json* field(const Json& object, std::string_view name) {
    const auto it = object.find(name);
    return it == object.end() ? nullptr : &*it;
}

const std::string* stringField(const Json& object, std::string_view name) {
    const auto* value = field(object, name);
    return value ? value->get_ptr<const std::string*>() : nullptr;
}

bool isString(const Json& object, std::string_view name, std::string_view expected) {
    const auto* value = stringField(object, name);
    return value && *value == expected;
}

Result<GenerationResult> parseClaims(const std::string& text) {
    const auto value = Json::parse(text, nullptr, false);
    const auto* claims = field(value, "claims");
    if (!value.is_object() || value.size() != 2 || !claims || !claims->is_array()) {
        return AppError::InvalidGeneration;
    }
    if (isString(value, "status", "insufficient_evidence") && claims->empty()) {
        return GenerationResult{};
    }
    if (!isString(value, "status", "answered") || claims->empty()) {
        return AppError::InvalidGeneration;
    }
    Answered answered;
    answered.claims.reserve(claims->size());
    for (const auto& claim : *claims) {
        const auto* statement = stringField(claim, "text");
        const auto* id = stringField(claim, "chunk_id");
        const auto* quote = stringField(claim, "quote");
        if (!claim.is_object() || claim.size() != 3 || !statement || !id || !quote) {
            return AppError::InvalidGeneration;
        }
        answered.claims.push_back({*statement, *id, *quote});
    }
    return GenerationResult{std::move(answered)};
}

}  // namespace rag
