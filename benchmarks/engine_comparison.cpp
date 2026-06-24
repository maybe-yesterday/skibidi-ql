#include "lexer.h"
#include "native_engine.h"
#include "optimizer.h"
#include "parser.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {

struct Options {
    std::string engine = "native";
    std::string workload = "scan";
    int iterations = 100;
    int rows = 10000;
    int bufferPages = 128;
};

std::size_t peakResidentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return 0;
    }
    return static_cast<std::size_t>(counters.PeakWorkingSetSize);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#ifdef __APPLE__
    return static_cast<std::size_t>(usage.ru_maxrss);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--engine") == 0 &&
            index + 1 < argc) {
            options.engine = argv[++index];
        } else if (std::strcmp(argv[index], "--workload") == 0 &&
                   index + 1 < argc) {
            options.workload = argv[++index];
        } else if (std::strcmp(argv[index], "--iterations") == 0 &&
                   index + 1 < argc) {
            options.iterations = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--rows") == 0 &&
                   index + 1 < argc) {
            options.rows = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--buffer-pages") == 0 &&
                   index + 1 < argc) {
            options.bufferPages = std::atoi(argv[++index]);
        } else {
            throw std::runtime_error("Invalid benchmark argument");
        }
    }
    if ((options.engine != "native" && options.engine != "sqlite") ||
        (options.workload != "point" &&
         options.workload != "scan" &&
         options.workload != "aggregate" &&
         options.workload != "join") ||
        options.iterations <= 0 || options.rows <= 0 ||
        options.bufferPages <= 0) {
        throw std::runtime_error("Invalid benchmark option value");
    }
    return options;
}

std::unique_ptr<ASTNode> parseOne(const std::string& source,
                                  const Catalog& catalog) {
    Parser parser(Lexer(source).tokenize());
    auto statements = parser.parseAll();
    if (statements.size() != 1) {
        throw std::runtime_error("Benchmark query must contain one statement");
    }
    OptimizationReport report;
    Optimizer optimizer(false, &catalog);
    return optimizer.optimize(std::move(statements.front()), report);
}

void createUsers(NativeEngine& engine) {
    CreateStmt create;
    create.table = "users";
    create.columns = {
        {"id", "INTEGER", true, true, "", ""},
        {"name", "TEXT", false, true, "", ""},
        {"active", "INTEGER", false, true, "", ""},
        {"category", "INTEGER", false, true, "", ""},
        {"score", "REAL", false, true, "", ""}
    };
    engine.execute(&create);
}

void insertNativeUsers(NativeEngine& engine, int rowCount) {
    constexpr int batchSize = 500;
    for (int first = 1; first <= rowCount; first += batchSize) {
        InsertStmt insert;
        insert.table = "users";
        const int last = std::min(rowCount, first + batchSize - 1);
        for (int id = first; id <= last; ++id) {
            std::vector<std::unique_ptr<ASTNode>> row;
            row.push_back(Literal::makeInt(id));
            row.push_back(
                Literal::makeString("user-" + std::to_string(id)));
            row.push_back(Literal::makeInt(id % 2));
            row.push_back(Literal::makeInt(id % 20));
            row.push_back(Literal::makeFloat(
                static_cast<double>(id % 1000) / 10.0));
            insert.valueRows.push_back(std::move(row));
        }
        engine.execute(&insert);
    }
}

void seedNativeJoin(NativeEngine& engine, int rowCount) {
    CreateStmt facts;
    facts.table = "facts";
    facts.columns = {
        {"id", "INTEGER", true, true, "", ""},
        {"d1_id", "INTEGER", false, true, "", ""},
        {"d2_id", "INTEGER", false, true, "", ""},
        {"amount", "INTEGER", false, true, "", ""}
    };
    engine.execute(&facts);
    CreateStmt d1;
    d1.table = "dimension_one";
    d1.columns = {
        {"id", "INTEGER", true, true, "", ""},
        {"label", "TEXT", false, true, "", ""}
    };
    engine.execute(&d1);
    CreateStmt d2 = d1;
    d2.table = "dimension_two";
    engine.execute(&d2);

    const int d1Rows = std::max(1, std::min(1000, rowCount / 5));
    const int d2Rows = 10;
    InsertStmt insertD1;
    insertD1.table = "dimension_one";
    for (int id = 1; id <= d1Rows; ++id) {
        std::vector<std::unique_ptr<ASTNode>> row;
        row.push_back(Literal::makeInt(id));
        row.push_back(Literal::makeString("d1-" + std::to_string(id)));
        insertD1.valueRows.push_back(std::move(row));
    }
    engine.execute(&insertD1);
    InsertStmt insertD2;
    insertD2.table = "dimension_two";
    for (int id = 1; id <= d2Rows; ++id) {
        std::vector<std::unique_ptr<ASTNode>> row;
        row.push_back(Literal::makeInt(id));
        row.push_back(Literal::makeString("d2-" + std::to_string(id)));
        insertD2.valueRows.push_back(std::move(row));
    }
    engine.execute(&insertD2);

    constexpr int batchSize = 500;
    for (int first = 1; first <= rowCount; first += batchSize) {
        InsertStmt insert;
        insert.table = "facts";
        const int last = std::min(rowCount, first + batchSize - 1);
        for (int id = first; id <= last; ++id) {
            std::vector<std::unique_ptr<ASTNode>> row;
            row.push_back(Literal::makeInt(id));
            row.push_back(Literal::makeInt((id - 1) % d1Rows + 1));
            row.push_back(Literal::makeInt((id - 1) % d2Rows + 1));
            row.push_back(Literal::makeInt(id % 100));
            insert.valueRows.push_back(std::move(row));
        }
        engine.execute(&insert);
    }
}

std::string nativeQuery(const Options& options) {
    if (options.workload == "point") {
        return "slay id, name no-cap users only-if id = " +
               std::to_string(options.rows / 2) + ";";
    }
    if (options.workload == "scan") {
        return "slay headcount(*) no-cap users only-if active = 1;";
    }
    if (options.workload == "aggregate") {
        return "slay category, stack(score) no-cap users "
               "only-if active = 1 vibe-check category;";
    }
    return "slay d2.label, stack(f.amount) "
           "no-cap facts lowkey f "
           "link-up dimension_one lowkey d1 "
           "fr-fr f.d1_id = d1.id "
           "link-up dimension_two lowkey d2 "
           "fr-fr f.d2_id = d2.id "
           "vibe-check d2.label;";
}

std::string sqliteQuery(const Options& options) {
    if (options.workload == "point") {
        return "SELECT id, name FROM users WHERE id = " +
               std::to_string(options.rows / 2);
    }
    if (options.workload == "scan") {
        return "SELECT COUNT(*) FROM users WHERE active = 1";
    }
    if (options.workload == "aggregate") {
        return "SELECT category, SUM(score) FROM users "
               "WHERE active = 1 GROUP BY category";
    }
    return "SELECT d2.label, SUM(f.amount) "
           "FROM facts AS f "
           "JOIN dimension_one AS d1 ON f.d1_id = d1.id "
           "JOIN dimension_two AS d2 ON f.d2_id = d2.id "
           "GROUP BY d2.label";
}

void sqliteExecute(sqlite3* database, const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error) !=
        SQLITE_OK) {
        const std::string message =
            error ? error : sqlite3_errmsg(database);
        if (error) sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void seedSqliteUsers(sqlite3* database, int rowCount) {
    sqliteExecute(database,
        "CREATE TABLE users("
        "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
        "active INTEGER NOT NULL, category INTEGER NOT NULL, "
        "score REAL NOT NULL)");
    sqliteExecute(database, "BEGIN");
    sqlite3_stmt* insert = nullptr;
    const char* sql =
        "INSERT INTO users VALUES(?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(database, sql, -1, &insert, nullptr) !=
        SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    for (int id = 1; id <= rowCount; ++id) {
        const std::string name = "user-" + std::to_string(id);
        sqlite3_bind_int(insert, 1, id);
        sqlite3_bind_text(insert, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 3, id % 2);
        sqlite3_bind_int(insert, 4, id % 20);
        sqlite3_bind_double(
            insert, 5, static_cast<double>(id % 1000) / 10.0);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
    }
    sqlite3_finalize(insert);
    sqliteExecute(database, "COMMIT");
}

void seedSqliteJoin(sqlite3* database, int rowCount) {
    sqliteExecute(database,
        "CREATE TABLE facts("
        "id INTEGER PRIMARY KEY, d1_id INTEGER NOT NULL, "
        "d2_id INTEGER NOT NULL, amount INTEGER NOT NULL);"
        "CREATE TABLE dimension_one("
        "id INTEGER PRIMARY KEY, label TEXT NOT NULL);"
        "CREATE TABLE dimension_two("
        "id INTEGER PRIMARY KEY, label TEXT NOT NULL);"
        "BEGIN");
    const int d1Rows = std::max(1, std::min(1000, rowCount / 5));
    sqlite3_stmt* dimension = nullptr;
    sqlite3_prepare_v2(
        database,
        "INSERT INTO dimension_one VALUES(?, ?)",
        -1, &dimension, nullptr);
    for (int id = 1; id <= d1Rows; ++id) {
        const std::string label = "d1-" + std::to_string(id);
        sqlite3_bind_int(dimension, 1, id);
        sqlite3_bind_text(
            dimension, 2, label.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(dimension);
        sqlite3_reset(dimension);
        sqlite3_clear_bindings(dimension);
    }
    sqlite3_finalize(dimension);
    sqlite3_prepare_v2(
        database,
        "INSERT INTO dimension_two VALUES(?, ?)",
        -1, &dimension, nullptr);
    for (int id = 1; id <= 10; ++id) {
        const std::string label = "d2-" + std::to_string(id);
        sqlite3_bind_int(dimension, 1, id);
        sqlite3_bind_text(
            dimension, 2, label.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(dimension);
        sqlite3_reset(dimension);
        sqlite3_clear_bindings(dimension);
    }
    sqlite3_finalize(dimension);

    sqlite3_stmt* fact = nullptr;
    sqlite3_prepare_v2(
        database,
        "INSERT INTO facts VALUES(?, ?, ?, ?)",
        -1, &fact, nullptr);
    for (int id = 1; id <= rowCount; ++id) {
        sqlite3_bind_int(fact, 1, id);
        sqlite3_bind_int(fact, 2, (id - 1) % d1Rows + 1);
        sqlite3_bind_int(fact, 3, (id - 1) % 10 + 1);
        sqlite3_bind_int(fact, 4, id % 100);
        sqlite3_step(fact);
        sqlite3_reset(fact);
        sqlite3_clear_bindings(fact);
    }
    sqlite3_finalize(fact);
    sqliteExecute(database, "COMMIT");
}

std::uint64_t consumeNative(const NativeQueryResult& result) {
    std::uint64_t checksum = result.rows.size();
    for (const auto& row : result.rows) {
        checksum += row.size();
        for (const auto& value : row) {
            checksum += value.toString().size();
        }
    }
    return checksum;
}

std::uint64_t consumeSqlite(sqlite3_stmt* statement) {
    std::uint64_t checksum = 0;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        ++checksum;
        const int columns = sqlite3_column_count(statement);
        checksum += static_cast<std::uint64_t>(columns);
        for (int column = 0; column < columns; ++column) {
            checksum += static_cast<std::uint64_t>(
                sqlite3_column_bytes(statement, column));
        }
    }
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(
            sqlite3_db_handle(statement)));
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return checksum;
}

void printResult(const Options& options,
                 double elapsedMs,
                 std::uint64_t checksum,
                 std::size_t engineMemory,
                 const NativeEngineStats* stats) {
    std::cout << std::fixed << std::setprecision(3)
              << "{"
              << "\"engine\":\"" << options.engine << "\","
              << "\"workload\":\"" << options.workload << "\","
              << "\"iterations\":" << options.iterations << ","
              << "\"rows\":" << options.rows << ","
              << "\"elapsed_ms\":" << elapsedMs << ","
              << "\"ops_per_sec\":"
              << options.iterations * 1000.0 / elapsedMs << ","
              << "\"peak_rss_bytes\":" << peakResidentBytes() << ","
              << "\"engine_memory_bytes\":" << engineMemory << ","
              << "\"checksum\":" << checksum;
    if (stats) {
        std::cout << ",\"vectorized_queries\":"
                  << stats->vectorizedQueries
                  << ",\"vector_batches\":" << stats->vectorBatches
                  << ",\"decoded_columns\":"
                  << stats->decodedColumns
                  << ",\"skipped_columns\":"
                  << stats->skippedColumns
                  << ",\"vector_nulls\":"
                  << stats->vectorNulls
                  << ",\"join_plans_enumerated\":"
                  << stats->joinPlansEnumerated
                  << ",\"join_order_changes\":"
                  << stats->joinOrderChanges
                  << ",\"hash_join_probes\":"
                  << stats->hashJoinProbes;
    }
    std::cout << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const auto root = std::filesystem::temp_directory_path() /
            ("skibidi-engine-comparison-" +
             std::to_string(
                 std::chrono::high_resolution_clock::now()
                     .time_since_epoch().count()));
        std::filesystem::create_directories(root);

        if (options.engine == "native") {
            NativeEngine engine(
                root / "native",
                static_cast<std::size_t>(options.bufferPages));
            if (options.workload == "join") {
                seedNativeJoin(engine, options.rows);
            } else {
                createUsers(engine);
                insertNativeUsers(engine, options.rows);
            }
            engine.flush();
            auto query = parseOne(nativeQuery(options), engine.catalog());
            (void)engine.execute(query.get());
            engine.resetStats();
            std::uint64_t checksum = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int iteration = 0;
                 iteration < options.iterations;
                 ++iteration) {
                checksum += consumeNative(engine.execute(query.get()));
            }
            const auto finish = std::chrono::steady_clock::now();
            const double elapsedMs =
                std::chrono::duration<double, std::milli>(
                    finish - start).count();
            const auto stats = engine.stats();
            printResult(
                options, elapsedMs, checksum,
                stats.residentPages * SlottedPage::PAGE_SIZE, &stats);
        } else {
            sqlite3* database = nullptr;
            const auto databasePath = (root / "sqlite.db").string();
            if (sqlite3_open(databasePath.c_str(), &database) != SQLITE_OK) {
                throw std::runtime_error("Could not open SQLite database");
            }
            sqliteExecute(database,
                "PRAGMA journal_mode=OFF;"
                "PRAGMA synchronous=OFF;"
                "PRAGMA temp_store=MEMORY");
            if (options.workload == "join") {
                seedSqliteJoin(database, options.rows);
            } else {
                seedSqliteUsers(database, options.rows);
            }
            sqlite3_stmt* statement = nullptr;
            const auto sql = sqliteQuery(options);
            if (sqlite3_prepare_v2(
                    database, sql.c_str(), -1, &statement, nullptr) !=
                SQLITE_OK) {
                throw std::runtime_error(sqlite3_errmsg(database));
            }
            (void)consumeSqlite(statement);
            sqlite3_memory_highwater(1);
            std::uint64_t checksum = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int iteration = 0;
                 iteration < options.iterations;
                 ++iteration) {
                checksum += consumeSqlite(statement);
            }
            const auto finish = std::chrono::steady_clock::now();
            const double elapsedMs =
                std::chrono::duration<double, std::milli>(
                    finish - start).count();
            const auto sqliteMemory = static_cast<std::size_t>(
                sqlite3_memory_highwater(0));
            sqlite3_finalize(statement);
            sqlite3_close(database);
            printResult(
                options, elapsedMs, checksum, sqliteMemory, nullptr);
        }

        std::error_code error;
        std::filesystem::remove_all(root, error);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine comparison error: "
                  << error.what() << "\n";
        return 1;
    }
}
