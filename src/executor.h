#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>

struct sqlite3;
struct sqlite3_stmt;

using SqliteRowCallback = int (*)(void*, int, char**, char**);
using SqliteStatementCallback = int (*)(void*, sqlite3_stmt*);

struct StatementCacheStats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

class SqliteExecutor {
public:
    explicit SqliteExecutor(sqlite3* database,
                            std::size_t cacheEntries = 128);
    ~SqliteExecutor();

    SqliteExecutor(const SqliteExecutor&) = delete;
    SqliteExecutor& operator=(const SqliteExecutor&) = delete;

    bool execute(const std::string& sql,
                 SqliteRowCallback callback = nullptr,
                 void* callbackContext = nullptr,
                 bool useCache = true,
                 std::string* error = nullptr);
    bool executeNative(const std::string& sql,
                       SqliteStatementCallback callback,
                       void* callbackContext = nullptr,
                       bool useCache = true,
                       std::string* error = nullptr);

    void clear();
    void resetStats();
    StatementCacheStats stats() const;

private:
    struct Entry {
        std::string sql;
        std::uint64_t hash = 0;
        sqlite3_stmt* statement = nullptr;
        std::size_t bytes = 0;
    };

    sqlite3* database_;
    std::size_t maxEntries_;
    std::size_t bytes_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t evictions_ = 0;
    std::list<Entry> lru_;
    std::unordered_multimap<std::uint64_t,
                            std::list<Entry>::iterator> entries_;

    static std::uint64_t hashSql(std::string_view sql);
    std::list<Entry>::iterator find(std::string_view sql,
                                    std::uint64_t hash);
    sqlite3_stmt* prepare(const std::string& sql, std::string* error);
    bool run(sqlite3_stmt* statement,
             SqliteRowCallback callback,
             SqliteStatementCallback nativeCallback,
             void* callbackContext,
             std::string* error);
    bool executeInternal(const std::string& sql,
                         SqliteRowCallback callback,
                         SqliteStatementCallback nativeCallback,
                         void* callbackContext,
                         bool useCache,
                         std::string* error);
    void erase(std::list<Entry>::iterator entry);
    void evictOldest();
};
