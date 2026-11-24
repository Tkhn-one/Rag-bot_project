#pragma once

#include "sqlite_helpers.hpp"

namespace rag::sql {

using TokenCallback = int (*)(void*, int, const char*, int, int, int);

// Query terms and document cut positions must use the same tokenizer as chunks_fts.
inline bool tokenizeUnicode61(sqlite3* db, std::string_view input, int flags, void* output, TokenCallback visit) {
    fts5_api* api = nullptr;
    auto getApi = prepare(db, "SELECT fts5(?1)");
    if (!getApi || sqlite3_bind_pointer(getApi.get(), 1, &api, "fts5_api_ptr", nullptr) != SQLITE_OK ||
        sqlite3_step(getApi.get()) != SQLITE_ROW || !api) return false;
    void* context = nullptr;
    fts5_tokenizer tokenizer{};
    Fts5Tokenizer* instance = nullptr;
    if (api->xFindTokenizer(api, "unicode61", &context, &tokenizer) != SQLITE_OK ||
        tokenizer.xCreate(context, nullptr, 0, &instance) != SQLITE_OK) return false;
    const std::unique_ptr<Fts5Tokenizer, decltype(tokenizer.xDelete)> owner(instance, tokenizer.xDelete);
    return std::in_range<int>(input.size()) &&
           tokenizer.xTokenize(instance, output, flags, input.data(), static_cast<int>(input.size()), visit) == SQLITE_OK;
}

}  // namespace rag::sql
