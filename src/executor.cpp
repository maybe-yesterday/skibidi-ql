#include "executor.h"

#include <sqlite3.h>

#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

SqliteExecutor::SqliteExecutor(sqlite3* database,
                               std::size_t cacheEntries)
    : database_(database), maxEntries_(cacheEntries) {}

SqliteExecutor::~SqliteExecutor() {
    clear();
}

bool SqliteExecutor::execute(const std::string& sql,
                             SqliteRowCallback callback,
                             void* callbackContext,
                             bool useCache,
                             std::string* error) {
    return executeInternal(sql,
                           callback,
                           nullptr,
                           callbackContext,
                           useCache,
                           error);
}

bool SqliteExecutor::executeNative(
    const std::string& sql,
    SqliteStatementCallback callback,
    void* callbackContext,
    bool useCache,
    std::string* error) {
    return executeInternal(sql,
                           nullptr,
                           callback,
                           callbackContext,
                           useCache,
                           error);
}

bool SqliteExecutor::executeInternal(
    const std::string& sql,
    SqliteRowCallback callback,
    SqliteStatementCallback nativeCallback,
    void* callbackContext,
    bool useCache,
    std::string* error) {
    if (!database_) {
        if (error) *error = "SQLite database is not open";
        return false;
    }

    if (!useCache || maxEntries_ == 0) {
        sqlite3_stmt* statement = prepare(sql, error);
        if (!statement) return false;
        const bool success =
            run(statement, callback, nativeCallback,
                callbackContext, error);
        sqlite3_finalize(statement);
        return success;
    }

    const std::uint64_t hash = hashSql(sql);
    auto entry = find(sql, hash);
    if (entry == lru_.end()) {
        ++misses_;
        sqlite3_stmt* statement = prepare(sql, error);
        if (!statement) return false;

        Entry newEntry;
        newEntry.sql = sql;
        newEntry.hash = hash;
        newEntry.statement = statement;
        newEntry.bytes =
            newEntry.sql.capacity() + 1 +
            static_cast<std::size_t>(
                sqlite3_stmt_status(statement,
                                    SQLITE_STMTSTATUS_MEMUSED,
                                    0));
        bytes_ += newEntry.bytes;
        lru_.push_front(std::move(newEntry));
        entries_.emplace(hash, lru_.begin());
        entry = lru_.begin();

        while (lru_.size() > maxEntries_) {
            evictOldest();
        }
    } else {
        ++hits_;
        lru_.splice(lru_.begin(), lru_, entry);
        entry = lru_.begin();
    }

    if (run(entry->statement, callback, nativeCallback,
            callbackContext, error)) {
        return true;
    }

    // SQLite may invalidate a prepared statement after a schema change.
    // Reprepare once before surfacing the error.
    if (sqlite3_errcode(database_) == SQLITE_SCHEMA) {
        erase(entry);
        sqlite3_stmt* statement = prepare(sql, error);
        if (!statement) return false;

        Entry replacement;
        replacement.sql = sql;
        replacement.hash = hash;
        replacement.statement = statement;
        replacement.bytes =
            replacement.sql.capacity() + 1 +
            static_cast<std::size_t>(
                sqlite3_stmt_status(statement,
                                    SQLITE_STMTSTATUS_MEMUSED,
                                    0));
        bytes_ += replacement.bytes;
        lru_.push_front(std::move(replacement));
        entries_.emplace(hash, lru_.begin());
        return run(statement, callback, nativeCallback,
                   callbackContext, error);
    }
    return false;
}

void SqliteExecutor::clear() {
    for (auto& entry : lru_) {
        if (entry.statement) sqlite3_finalize(entry.statement);
    }
    lru_.clear();
    entries_.clear();
    bytes_ = 0;
}

void SqliteExecutor::resetStats() {
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
}

StatementCacheStats SqliteExecutor::stats() const {
    StatementCacheStats result;
    result.hits = hits_;
    result.misses = misses_;
    result.evictions = evictions_;
    result.entries = lru_.size();
    result.bytes = bytes_;
    return result;
}

std::uint64_t SqliteExecutor::hashSql(std::string_view sql) {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (unsigned char ch : sql) {
        hash ^= ch;
        hash *= prime;
    }
    return hash;
}

std::list<SqliteExecutor::Entry>::iterator SqliteExecutor::find(
    std::string_view sql,
    std::uint64_t hash) {
    const auto range = entries_.equal_range(hash);
    for (auto candidate = range.first;
         candidate != range.second;
         ++candidate) {
        const auto entry = candidate->second;
        if (entry->sql.size() == sql.size() &&
            std::memcmp(entry->sql.data(),
                        sql.data(),
                        sql.size()) == 0) {
            return entry;
        }
    }
    return lru_.end();
}

sqlite3_stmt* SqliteExecutor::prepare(const std::string& sql,
                                      std::string* error) {
    sqlite3_stmt* statement = nullptr;
    const int rc = sqlite3_prepare_v3(database_,
                                      sql.c_str(),
                                      -1,
                                      SQLITE_PREPARE_PERSISTENT,
                                      &statement,
                                      nullptr);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(database_);
        if (statement) sqlite3_finalize(statement);
        return nullptr;
    }
    return statement;
}

bool SqliteExecutor::run(sqlite3_stmt* statement,
                         SqliteRowCallback callback,
                         SqliteStatementCallback nativeCallback,
                         void* callbackContext,
                         std::string* error) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);

    std::vector<char*> values;
    std::vector<char*> names;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        if (nativeCallback) {
            if (nativeCallback(callbackContext, statement) != 0) {
                sqlite3_reset(statement);
                if (error) {
                    *error =
                        "SQLite native callback aborted execution";
                }
                return false;
            }
            continue;
        }
        if (!callback) continue;

        const int columns = sqlite3_column_count(statement);
        if (values.size() != static_cast<std::size_t>(columns)) {
            values.resize(static_cast<std::size_t>(columns));
            names.resize(static_cast<std::size_t>(columns));
            for (int column = 0; column < columns; ++column) {
                names[static_cast<std::size_t>(column)] =
                    const_cast<char*>(
                        sqlite3_column_name(statement, column));
            }
        }
        for (int column = 0; column < columns; ++column) {
            values[static_cast<std::size_t>(column)] =
                reinterpret_cast<char*>(
                    const_cast<unsigned char*>(
                        sqlite3_column_text(statement, column)));
        }
        if (callback(callbackContext,
                     columns,
                     values.data(),
                     names.data()) != 0) {
            sqlite3_reset(statement);
            if (error) *error = "SQLite row callback aborted execution";
            return false;
        }
    }

    if (rc != SQLITE_DONE) {
        if (error) *error = sqlite3_errmsg(database_);
        sqlite3_reset(statement);
        return false;
    }
    sqlite3_reset(statement);
    return true;
}

void SqliteExecutor::erase(std::list<Entry>::iterator entry) {
    bytes_ -= entry->bytes;
    const auto range = entries_.equal_range(entry->hash);
    for (auto candidate = range.first;
         candidate != range.second;
         ++candidate) {
        if (candidate->second == entry) {
            entries_.erase(candidate);
            break;
        }
    }
    if (entry->statement) sqlite3_finalize(entry->statement);
    lru_.erase(entry);
}

void SqliteExecutor::evictOldest() {
    if (lru_.empty()) return;
    erase(std::prev(lru_.end()));
    ++evictions_;
}
