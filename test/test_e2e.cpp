#include "test_framework.h"

#include "compiler.h"
#include "metadata.h"

#include <sqlite3.h>

#include <string>
#include <vector>

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

static void execute(sqlite3* db, const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : "SQLite error";
        if (error) sqlite3_free(error);
        throw std::runtime_error(message + " in SQL: " + sql);
    }
}

static std::vector<std::vector<std::string>> query(sqlite3* db,
                                                   const std::string& sql) {
    std::vector<std::vector<std::string>> rows;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) !=
        SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }

    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        std::vector<std::string> row;
        for (int column = 0;
             column < sqlite3_column_count(statement);
             ++column) {
            const unsigned char* value = sqlite3_column_text(statement, column);
            row.emplace_back(value ? reinterpret_cast<const char*>(value)
                                   : "NULL");
        }
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db);
        sqlite3_finalize(statement);
        throw std::runtime_error(message);
    }
    sqlite3_finalize(statement);
    return rows;
}

static Catalog usersCatalog() {
    Catalog catalog;
    TableMeta users;
    users.name = "users";

    ColumnMeta id;
    id.name = "id";
    id.type = "INTEGER";
    id.primary_key = true;

    ColumnMeta name;
    name.name = "name";
    name.type = "TEXT";
    name.not_null = true;

    ColumnMeta active;
    active.name = "active";
    active.type = "INTEGER";
    active.not_null = true;

    users.columns.push_back(std::move(id));
    users.columns.push_back(std::move(name));
    users.columns.push_back(std::move(active));
    catalog.addTable(users);
    return catalog;
}

TEST(full_pipeline_executes_select_and_cache_hit) {
    Database database;
    execute(database.handle,
            "CREATE TABLE users (id INTEGER PRIMARY KEY, "
            "name TEXT NOT NULL, active INTEGER NOT NULL)");
    execute(database.handle,
            "INSERT INTO users VALUES "
            "(1, 'Ada', 1), (2, 'Grace', 1), (3, 'Linus', 0)");

    Catalog catalog = usersCatalog();
    QueryCompiler compiler;
    const std::string source =
        "slay unique-fr id, name no-cap users only-if id = 2 "
        "hits-different name;";

    auto first = compiler.compile(source, catalog);
    ASSERT_FALSE(first.cacheHit);
    ASSERT_EQ(first.statements.size(), (size_t)1);
    ASSERT_FALSE(first.statements[0].sql.find("DISTINCT") != std::string::npos);
    ASSERT_FALSE(first.statements[0].sql.find("ORDER BY") != std::string::npos);
    ASSERT_CONTAINS(first.statements[0].sql, "LIMIT 1");

    const auto firstRows = query(database.handle, first.statements[0].sql);
    ASSERT_EQ(firstRows.size(), (size_t)1);
    ASSERT_EQ(firstRows[0][0], std::string("2"));
    ASSERT_EQ(firstRows[0][1], std::string("Grace"));

    auto second = compiler.compile(source, catalog);
    ASSERT_TRUE(second.cacheHit);
    const auto secondRows = query(database.handle, second.statements[0].sql);
    ASSERT_EQ(secondRows.size(), firstRows.size());
    ASSERT_EQ(secondRows[0].size(), firstRows[0].size());
    ASSERT_EQ(secondRows[0][0], firstRows[0][0]);
    ASSERT_EQ(secondRows[0][1], firstRows[0][1]);
}

TEST(full_pipeline_executes_dml_and_aggregate) {
    Database database;
    execute(database.handle,
            "CREATE TABLE users (id INTEGER PRIMARY KEY, "
            "name TEXT NOT NULL, active INTEGER NOT NULL)");

    Catalog catalog = usersCatalog();
    QueryCompiler compiler;
    const std::string source =
        "yeet-into users (id, name, active) drip "
        "(1, 'Ada', 1), (2, 'Grace', 1), (3, 'Linus', 0);"
        "glow-up users be-like active = 1 only-if id = 3;"
        "slay headcount(name) lowkey total no-cap users only-if active = 1;";

    auto compilation = compiler.compile(source, catalog);
    ASSERT_EQ(compilation.statements.size(), (size_t)3);
    for (std::size_t i = 0; i < 2; ++i) {
        execute(database.handle, compilation.statements[i].sql);
    }

    ASSERT_CONTAINS(compilation.statements[2].sql, "COUNT(*)");
    const auto rows =
        query(database.handle, compilation.statements[2].sql);
    ASSERT_EQ(rows.size(), (size_t)1);
    ASSERT_EQ(rows[0][0], std::string("3"));
}

int main() {
    return run_all_tests("End-to-end");
}
