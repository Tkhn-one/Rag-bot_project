#pragma once

#include "rag/domain/model.hpp"
#include <sqlite3.h>
#include <memory>
#include <string_view>

namespace rag::sql {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

inline Statement prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    return Statement(statement, sqlite3_finalize);
}

inline bool execute(sqlite3* database, const char* sql) {
    return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

enum class WriteMode { Immediate, AllowSavepoint };

class WriteTransaction {
public:
    explicit WriteTransaction(sqlite3* database, WriteMode mode = WriteMode::Immediate)
        : database_(database),
          nested_(mode == WriteMode::AllowSavepoint && sqlite3_get_autocommit(database_) == 0),
          active_(execute(database_, nested_ ? "SAVEPOINT answer_write" : "BEGIN IMMEDIATE")) {}

    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;

    ~WriteTransaction() {
        if (active_) {
            execute(database_, nested_ ? "ROLLBACK TO answer_write; RELEASE answer_write" : "ROLLBACK");
        }
    }

    bool started() const { return active_; }

    bool commit() {
        if (!execute(database_, nested_ ? "RELEASE answer_write" : "COMMIT")) {
            return false;
        }
        active_ = false;
        return true;
    }

private:
    sqlite3* database_;
    bool nested_;
    bool active_;
};

inline AppError databaseError(sqlite3* database, AppError fallback) {
    const auto code = sqlite3_errcode(database);
    if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB) {
        return AppError::StoreCorrupt;
    }
    if (code == SQLITE_BUSY || code == SQLITE_LOCKED) {
        return AppError::LibraryBusy;
    }
    return fallback;
}

inline std::optional<std::string> readText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    if (!value) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

inline bool bindText(sqlite3_stmt* statement, int index, std::string_view text) {
    return sqlite3_bind_text64(statement, index, text.empty() ? "" : text.data(), text.size(), SQLITE_TRANSIENT,
                               SQLITE_UTF8) == SQLITE_OK;
}

}  // namespace rag::sql
