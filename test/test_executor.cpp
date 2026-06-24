#include "test_framework.h"

#include "executor.h"

#include <sqlite3.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

struct Database {
    sqlite3* handle = nullptr;

    Database() {
        if (sqlite3_open(":memory:", &handle) != SQLITE_OK) {
            throw std::runtime_error("Could not open SQLite database");
        }
    }

    ~Database() {
        if (handle) sqlite3_close(handle);
    }
};

static int sumFirstColumn(void* context,
                          int columns,
                          char** values,
                          char** /* names */) {
    if (columns > 0 && values[0]) {
        *static_cast<long long*>(context) +=
            std::strtoll(values[0], nullptr, 10);
    }
    return 0;
}

TEST(prepared_statement_cache_hits_on_repeated_query) {
    Database database;
    SqliteExecutor executor(database.handle, 4);
    std::string error;
    ASSERT_TRUE(executor.execute(
        "CREATE TABLE t (id INTEGER PRIMARY KEY)", nullptr, nullptr,
        false, &error));
    ASSERT_TRUE(executor.execute(
        "INSERT INTO t VALUES (1), (2)", nullptr, nullptr, false, &error));

    long long sum = 0;
    ASSERT_TRUE(executor.execute(
        "SELECT id FROM t ORDER BY id", sumFirstColumn, &sum, true, &error));
    ASSERT_TRUE(executor.execute(
        "SELECT id FROM t ORDER BY id", sumFirstColumn, &sum, true, &error));

    ASSERT_EQ(sum, (long long)6);
    const auto stats = executor.stats();
    ASSERT_EQ(stats.misses, (size_t)1);
    ASSERT_EQ(stats.hits, (size_t)1);
    ASSERT_EQ(stats.entries, (size_t)1);
    ASSERT_TRUE(stats.bytes > 0);
}

TEST(prepared_statement_cache_evicts_lru) {
    Database database;
    SqliteExecutor executor(database.handle, 2);
    std::string error;

    ASSERT_TRUE(executor.execute("SELECT 1", nullptr, nullptr, true, &error));
    ASSERT_TRUE(executor.execute("SELECT 2", nullptr, nullptr, true, &error));
    ASSERT_TRUE(executor.execute("SELECT 1", nullptr, nullptr, true, &error));
    ASSERT_TRUE(executor.execute("SELECT 3", nullptr, nullptr, true, &error));
    ASSERT_TRUE(executor.execute("SELECT 2", nullptr, nullptr, true, &error));

    const auto stats = executor.stats();
    ASSERT_EQ(stats.evictions, (size_t)2);
    ASSERT_EQ(stats.entries, (size_t)2);
}

TEST(disabled_statement_cache_does_not_store_statement) {
    Database database;
    SqliteExecutor executor(database.handle, 4);
    std::string error;
    ASSERT_TRUE(executor.execute(
        "SELECT 1", nullptr, nullptr, false, &error));
    ASSERT_TRUE(executor.execute(
        "SELECT 1", nullptr, nullptr, false, &error));

    const auto stats = executor.stats();
    ASSERT_EQ(stats.entries, (size_t)0);
    ASSERT_EQ(stats.hits, (size_t)0);
    ASSERT_EQ(stats.misses, (size_t)0);
}

TEST(clear_finalizes_all_cached_statements) {
    Database database;
    SqliteExecutor executor(database.handle, 4);
    std::string error;
    ASSERT_TRUE(executor.execute(
        "SELECT 1", nullptr, nullptr, true, &error));
    ASSERT_TRUE(executor.execute(
        "SELECT 2", nullptr, nullptr, true, &error));

    executor.clear();
    ASSERT_EQ(executor.stats().entries, (size_t)0);
    ASSERT_EQ(executor.stats().bytes, (size_t)0);
    ASSERT_TRUE(executor.execute(
        "SELECT 1", nullptr, nullptr, true, &error));
    ASSERT_EQ(executor.stats().misses, (size_t)3);
}

int main() {
    return run_all_tests("Executor");
}
