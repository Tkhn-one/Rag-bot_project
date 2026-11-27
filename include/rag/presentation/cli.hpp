#pragma once

#include "rag/domain/model.hpp"

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace rag {

class IndexingService;
class RagService;
class IHistoryStore;
class ICitationSource;
class IKnowledgeStore;

std::string_view describeError(AppError error);
int runSourceCli(IHistoryStore& history, ICitationSource& sources, std::size_t entry, std::size_t claim,
                 std::ostream& output, std::ostream& errors);
int runHistoryCli(IHistoryStore& store, bool clear, std::ostream& output, std::ostream& errors);
int runPolicyCli(IKnowledgeStore& store, std::optional<DocumentId> document, std::optional<DocumentPolicy> policy,
                 std::ostream& output, std::ostream& errors);

int runCli(IndexingService& index, RagService& rag, std::span<const std::filesystem::path> files,
           std::string question, const ModelProfile& profile, std::ostream& output, std::ostream& errors);

}  // namespace rag
