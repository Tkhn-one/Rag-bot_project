#pragma once

#include "rag/application/ports.hpp"

struct sqlite3;

namespace rag {

inline constexpr auto createSearchIndex = R"sql(
    CREATE VIRTUAL TABLE chunks_fts USING fts5(text, document_id UNINDEXED, line UNINDEXED, tokenize='unicode61');
    INSERT INTO chunks_fts(text, document_id, line) SELECT text, document_id, line FROM chunks;
    CREATE TABLE search_profile (
        id INTEGER PRIMARY KEY CHECK(id=1), model TEXT NOT NULL, dimension INTEGER NOT NULL,
        settings TEXT NOT NULL
    ) STRICT;
    INSERT INTO search_profile VALUES (1, '', 0, 'unicode61-bm25-cos-rrf-v1/20/60/8/2');
    CREATE TABLE embeddings (
        document_id INTEGER NOT NULL, line INTEGER NOT NULL, model TEXT NOT NULL,
        dimension INTEGER NOT NULL CHECK(dimension > 0), vector BLOB NOT NULL CHECK(length(vector)=4*dimension),
        PRIMARY KEY(document_id,line),
        FOREIGN KEY(document_id,line) REFERENCES chunks(document_id,line) ON DELETE CASCADE
    ) STRICT, WITHOUT ROWID;
    CREATE TRIGGER index_chunk AFTER INSERT ON chunks BEGIN
        INSERT INTO chunks_fts(text,document_id,line) VALUES(NEW.text,NEW.document_id,NEW.line);
    END;
    PRAGMA user_version=6;
)sql";

Result<bool> searchProfileMatches(sqlite3* db, const IEmbeddingProvider* provider, SearchSettings settings);
// Caller holds the publication transaction. Re-slice the original blocks with the selected tokenizer.
Result<bool> rebuildSearchChunks(sqlite3* db, const IEmbeddingProvider* provider);
Result<bool> indexVector(sqlite3* db, const IEmbeddingProvider& provider, std::int64_t id,
                         std::size_t line, std::string_view text);

}  // namespace rag
