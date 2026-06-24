#include "compiler.h"
#include "executor.h"
#include "metadata.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

struct Options {
    std::string mode = "sql";
    std::string workload = "point";
    int iterations = 5000;
    int rows = 10000;
};

struct WorkloadQuery {
    std::string sql;
    std::string skibidi;
};

struct RunMetrics {
    std::uint64_t checksum = 0;
    std::uint64_t compileNanoseconds = 0;
    std::uint64_t prepareNanoseconds = 0;
    std::uint64_t executeNanoseconds = 0;
};

class Database {
public:
    Database() {
        if (sqlite3_open(":memory:", &handle_) != SQLITE_OK) {
            throw std::runtime_error("Could not open SQLite database");
        }
    }

    ~Database() {
        if (handle_) sqlite3_close(handle_);
    }

    sqlite3* get() const { return handle_; }

private:
    sqlite3* handle_ = nullptr;
};

static void execute(sqlite3* db, const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        if (error) sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

static void seed(sqlite3* db, int rows) {
    execute(db,
            "CREATE TABLE users ("
            "id INTEGER PRIMARY KEY, "
            "name TEXT NOT NULL, "
            "active INTEGER NOT NULL, "
            "category INTEGER NOT NULL, "
            "score REAL NOT NULL)");
    execute(db, "BEGIN");

    sqlite3_stmt* insert = nullptr;
    const char* sql =
        "INSERT INTO users(id, name, active, category, score) "
        "VALUES (?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &insert, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }

    for (int id = 1; id <= rows; ++id) {
        const std::string name = "user-" + std::to_string(id);
        sqlite3_bind_int(insert, 1, id);
        sqlite3_bind_text(insert, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 3, id % 2);
        sqlite3_bind_int(insert, 4, id % 20);
        sqlite3_bind_double(insert, 5, static_cast<double>(id % 1000) / 10.0);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            sqlite3_finalize(insert);
            throw std::runtime_error(sqlite3_errmsg(db));
        }
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
    }
    sqlite3_finalize(insert);
    execute(db, "COMMIT");
}

static Catalog benchmarkCatalog() {
    Catalog catalog;
    TableMeta users;
    users.name = "users";

    auto add = [&](const std::string& name,
                   const std::string& type,
                   bool primaryKey,
                   bool notNull) {
        ColumnMeta column;
        column.name = name;
        column.type = type;
        column.primary_key = primaryKey;
        column.not_null = notNull;
        users.columns.push_back(std::move(column));
    };

    add("id", "INTEGER", true, true);
    add("name", "TEXT", false, true);
    add("active", "INTEGER", false, true);
    add("category", "INTEGER", false, true);
    add("score", "REAL", false, true);
    catalog.addTable(users);
    return catalog;
}

static std::vector<WorkloadQuery> makeWorkload(const Options& options) {
    const int id = options.rows / 2;
    WorkloadQuery point{
        "SELECT id, name FROM users WHERE id = " + std::to_string(id) +
            " LIMIT 1",
        "slay unique-fr id, name no-cap users only-if id = " +
            std::to_string(id) + " hits-different name;"};
    WorkloadQuery count{
        "SELECT COUNT(*) FROM users WHERE active = 1",
        "slay headcount(name) no-cap users only-if active = 1;"};
    WorkloadQuery aggregate{
        "SELECT category, SUM(score) FROM users "
        "WHERE active = 1 GROUP BY category",
        "slay category, stack(score) no-cap users "
        "only-if active = 1 vibe-check category;"};

    if (options.workload == "point") return {point};
    if (options.workload == "count") return {count};
    if (options.workload == "mixed") return {point, count, aggregate};
    throw std::runtime_error("Unknown workload: " + options.workload);
}

static RunMetrics executeAndConsume(sqlite3* db, const std::string& sql) {
    RunMetrics metrics;
    sqlite3_stmt* statement = nullptr;
    const auto prepareStart = std::chrono::steady_clock::now();
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) !=
        SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    const auto prepareFinish = std::chrono::steady_clock::now();
    metrics.prepareNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            prepareFinish - prepareStart).count();

    const auto executeStart = std::chrono::steady_clock::now();
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        const int columns = sqlite3_column_count(statement);
        metrics.checksum += static_cast<std::uint64_t>(columns);
        for (int column = 0; column < columns; ++column) {
            metrics.checksum += static_cast<std::uint64_t>(
                sqlite3_column_bytes(statement, column));
            metrics.checksum += static_cast<std::uint64_t>(
                sqlite3_column_int64(statement, column));
        }
    }
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(statement);
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    sqlite3_finalize(statement);
    const auto executeFinish = std::chrono::steady_clock::now();
    metrics.executeNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            executeFinish - executeStart).count();
    return metrics;
}

static int consumeNativeRow(void* context, sqlite3_stmt* statement) {
    auto* checksum = static_cast<std::uint64_t*>(context);
    const int columns = sqlite3_column_count(statement);
    *checksum += static_cast<std::uint64_t>(columns);
    for (int column = 0; column < columns; ++column) {
        *checksum += static_cast<std::uint64_t>(
            sqlite3_column_bytes(statement, column));
        *checksum += static_cast<std::uint64_t>(
            sqlite3_column_int64(statement, column));
    }
    return 0;
}

static RunMetrics executePreparedAndConsume(SqliteExecutor& executor,
                                            const std::string& sql) {
    RunMetrics metrics;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    if (!executor.executeNative(sql,
                                consumeNativeRow,
                                &metrics.checksum,
                                true,
                                &error)) {
        throw std::runtime_error(error);
    }
    const auto finish = std::chrono::steady_clock::now();
    metrics.executeNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            finish - start).count();
    return metrics;
}

static Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            options.mode = argv[++i];
        } else if (std::strcmp(argv[i], "--workload") == 0 &&
                   i + 1 < argc) {
            options.workload = argv[++i];
        } else if (std::strcmp(argv[i], "--iterations") == 0 &&
                   i + 1 < argc) {
            options.iterations = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--rows") == 0 && i + 1 < argc) {
            options.rows = std::atoi(argv[++i]);
        } else {
            throw std::runtime_error("Invalid benchmark argument");
        }
    }
    if (options.iterations <= 0 || options.rows <= 0) {
        throw std::runtime_error("Iterations and rows must be positive");
    }
    if (options.mode != "sql" &&
        options.mode != "sql-prepared" &&
        options.mode != "skibidi-uncached" &&
        options.mode != "skibidi-cached" &&
        options.mode != "skibidi-prepared") {
        throw std::runtime_error("Unknown benchmark mode: " + options.mode);
    }
    return options;
}

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        Database database;
        seed(database.get(), options.rows);
        Catalog catalog = benchmarkCatalog();
        QueryCompiler compiler(64, 2 * 1024 * 1024);
        SqliteExecutor statementExecutor(database.get(), 64);
        const auto workload = makeWorkload(options);
        const bool useSkibidi =
            options.mode == "skibidi-uncached" ||
            options.mode == "skibidi-cached" ||
            options.mode == "skibidi-prepared";
        const bool useCache =
            options.mode == "skibidi-cached" ||
            options.mode == "skibidi-prepared";
        const bool usePrepared =
            options.mode == "sql-prepared" ||
            options.mode == "skibidi-prepared";

        auto runQuery = [&](int index) {
            RunMetrics metrics;
            const auto& query =
                workload[static_cast<std::size_t>(index) % workload.size()];
            if (!useSkibidi) {
                return usePrepared
                    ? executePreparedAndConsume(statementExecutor, query.sql)
                    : executeAndConsume(database.get(), query.sql);
            }
            const auto compileStart = std::chrono::steady_clock::now();
            auto compilation =
                compiler.compileFast(query.skibidi, catalog, useCache);
            const auto compileFinish = std::chrono::steady_clock::now();
            const auto& sql =
                compilation.output->statements.front().sql;
            metrics = usePrepared
                ? executePreparedAndConsume(statementExecutor, sql)
                : executeAndConsume(database.get(), sql);
            metrics.compileNanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    compileFinish - compileStart).count();
            return metrics;
        };

        for (std::size_t i = 0; i < workload.size(); ++i) {
            (void)runQuery(static_cast<int>(i));
        }
        compiler.resetCacheStats();
        statementExecutor.resetStats();
        sqlite3_memory_highwater(1);

        RunMetrics totals;
        const auto start = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < options.iterations; ++iteration) {
            const RunMetrics metrics = runQuery(iteration);
            totals.checksum += metrics.checksum;
            totals.compileNanoseconds += metrics.compileNanoseconds;
            totals.prepareNanoseconds += metrics.prepareNanoseconds;
            totals.executeNanoseconds += metrics.executeNanoseconds;
        }
        const auto finish = std::chrono::steady_clock::now();
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(finish - start).count();
        const double operationsPerSecond =
            static_cast<double>(options.iterations) * 1000.0 / elapsedMs;
        const CacheStats cache = compiler.cacheStats();
        const StatementCacheStats statementCache =
            statementExecutor.stats();

        std::cout << std::fixed << std::setprecision(3)
                  << "{"
#ifdef NDEBUG
                  << "\"release_build\":true,"
#else
                  << "\"release_build\":false,"
#endif
                  << "\"mode\":\"" << options.mode << "\","
                  << "\"workload\":\"" << options.workload << "\","
                  << "\"iterations\":" << options.iterations << ","
                  << "\"rows\":" << options.rows << ","
                  << "\"elapsed_ms\":" << elapsedMs << ","
                  << "\"compile_ms\":"
                  << totals.compileNanoseconds / 1000000.0 << ","
                  << "\"sqlite_prepare_ms\":"
                  << totals.prepareNanoseconds / 1000000.0 << ","
                  << "\"sqlite_execute_ms\":"
                  << totals.executeNanoseconds / 1000000.0 << ","
                  << "\"ops_per_sec\":" << operationsPerSecond << ","
                  << "\"sqlite_peak_bytes\":"
                  << sqlite3_memory_highwater(0) << ","
                  << "\"sqlite_current_bytes\":"
                  << sqlite3_memory_used() << ","
                  << "\"cache_hits\":" << cache.hits << ","
                  << "\"cache_misses\":" << cache.misses << ","
                  << "\"cache_bytes\":" << cache.bytes << ","
                  << "\"statement_cache_hits\":"
                  << statementCache.hits << ","
                  << "\"statement_cache_misses\":"
                  << statementCache.misses << ","
                  << "\"statement_cache_bytes\":"
                  << statementCache.bytes << ","
                  << "\"checksum\":" << totals.checksum
                  << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark error: " << error.what() << "\n";
        return 1;
    }
}
