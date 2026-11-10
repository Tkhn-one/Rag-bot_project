#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

namespace rag {

class IndexingService;
class RagService;

int runCli(IndexingService& index, RagService& rag, const std::filesystem::path& file,
           std::string question, std::ostream& output, std::ostream& errors);

}  // namespace rag
