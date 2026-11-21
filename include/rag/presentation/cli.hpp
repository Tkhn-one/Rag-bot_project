#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>

namespace rag {

class IndexingService;
class RagService;

int runCli(IndexingService& index, RagService& rag, std::span<const std::filesystem::path> files,
           std::string question, std::ostream& output, std::ostream& errors);

}  // namespace rag
