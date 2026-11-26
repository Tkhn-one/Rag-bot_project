#pragma once

#include "rag/application/ports.hpp"
#include <nlohmann/json.hpp>

namespace rag {
const nlohmann::json* field(const nlohmann::json& object, std::string_view name);
const std::string* stringField(const nlohmann::json& object, std::string_view name);
bool isString(const nlohmann::json& object, std::string_view name, std::string_view expected);
std::string generationInput(const GenerationRequest& request, std::size_t count);
Result<GenerationResult> parseClaims(const std::string& text);
}  // namespace rag
