#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace rag {

class IndexingService;
class RagService;
enum class AppError;

std::string_view describeError(AppError error);

int runCli(IndexingService& index, RagService& rag, std::span<const std::filesystem::path> files,
           std::string question, std::ostream& output, std::ostream& errors);

}  // namespace rag
