#pragma once

#include "rag/application/ports.hpp"
#include "rag/application/block_slices.hpp"

#include <algorithm>
#include <random>

namespace rag {

class TxtReader final : public IDocumentReader {
public:
    Result<ExtractedDocument> read(const std::filesystem::path& path) const override;
};

class PdfReader final : public IDocumentReader {
public:
    Result<ExtractedDocument> read(const std::filesystem::path& path) const override;
};

class DocxReader final : public IDocumentReader {
public:
    Result<ExtractedDocument> read(const std::filesystem::path& path) const override;
};

class InMemoryKnowledgeStore final : public IKnowledgeStore {
public:
    InMemoryKnowledgeStore() {
        std::random_device random;
        std::uniform_int_distribution<int> digit(0, 15);
        for (int i = 0; i < 32; ++i) {
            revision_.libraryId += "0123456789abcdef"[digit(random)];
        }
    }
    InMemoryKnowledgeStore(const InMemoryKnowledgeStore&) = delete;
    InMemoryKnowledgeStore& operator=(const InMemoryKnowledgeStore&) = delete;

    Result<std::size_t> save(std::string source, std::string content,
                             std::vector<DocumentLine> lines, DocumentPolicy policy,
                             std::string positionLayout = {}) override {
        if (lines.empty()) {
            return AppError::StoreWriteFailed;
        }
        const auto duplicate = std::ranges::find_if(documents_, [&](const IndexedDocument& doc) {
            return doc.content() == content && doc.positionLayout() == positionLayout;
        });
        if (duplicate != documents_.end()) {
            return duplicate->chunks().size();
        }

        std::ranges::sort(lines, {}, &DocumentLine::number);
        if (lines.front().number == 0 ||
            std::ranges::adjacent_find(lines, {}, &DocumentLine::number) != lines.end()) {
            return AppError::StoreWriteFailed;
        }
        const DocumentId id = documents_.size() + 1;
        std::vector<Chunk> chunks;
        chunks.reserve(lines.size());
        std::size_t chunkId = 0;
        for (const auto& line : lines) {
            const auto slices = splitBlock(line.text, nullptr);
            if (const auto* error = std::get_if<AppError>(&slices)) return *error;
            for (const auto& piece : std::get<std::vector<BlockSlice>>(slices)) {
                chunkId = std::max(chunkId + 1, line.number);
                chunks.emplace_back(id, line.number, std::string(piece.text), line.page, line.docx, chunkId, piece.characters);
            }
        }
        documents_.emplace_back(id, std::move(source), std::move(content), std::move(chunks),
                                policy, std::move(positionLayout));
        ++revision_.number;
        return documents_.back().chunks().size();
    }

    Result<LibrarySnapshot> snapshot() const override {
        return LibrarySnapshot{revision_, policy_, documents_};
    }

    Result<bool> setLibraryPolicy(DocumentPolicy policy) override {
        if (policy_ == policy) {
            return false;
        }
        policy_ = policy;
        ++revision_.number;
        return true;
    }

    Result<bool> setDocumentPolicy(DocumentId id, DocumentPolicy policy) override {
        const auto document = std::ranges::find(documents_, id, &IndexedDocument::id);
        if (document == documents_.end()) {
            return AppError::StoreWriteFailed;
        }
        if (document->policy() == policy) {
            return false;
        }
        document->setPolicy(policy);
        ++revision_.number;
        return true;
    }

private:
    IndexRevision revision_{};
    DocumentPolicy policy_ = DocumentPolicy::LocalOnly;
    std::vector<IndexedDocument> documents_;
};

class KeywordRetriever final : public IRetriever {
public:
    Result<std::vector<Chunk>> retrieve(const Question& question, const LibrarySnapshot& library) const override;
};

class MockProvider final : public ILlmProvider {
public:
    ModelProfile profile() const override { return {ProcessingType::Local, "mock-extractive-v1"}; }

    Result<ProposedCitation> generate(const GenerationRequest& request) override {
        if (request.evidence.empty()) {
            return AppError::InvalidCitation;
        }
        return ProposedCitation::fromChunk(request.evidence.front());
    }
};

}  // namespace rag
