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
    int bufferPages = 1024;
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

bool isJoinWorkload(const std::string& workload) {
    return workload == "join" || workload == "join_miss";
}

bool isContextWorkload(const std::string& workload) {
    return workload == "context_schema" ||
           workload == "context_spill" ||
           workload == "context_spill_acl" ||
           workload == "context_objects";
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
         options.workload != "count_miss" &&
         options.workload != "join" &&
         options.workload != "join_miss" &&
         options.workload != "context_schema" &&
         options.workload != "context_spill" &&
         options.workload != "context_spill_acl" &&
         options.workload != "context_objects") ||
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

void seedNativeJoin(NativeEngine& engine,
                    int rowCount,
                    bool missDimensionTwo = false) {
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
            row.push_back(Literal::makeInt(
                missDimensionTwo
                    ? d2Rows + 1000 + (id % d2Rows)
                    : (id - 1) % d2Rows + 1));
            row.push_back(Literal::makeInt(id % 100));
            insert.valueRows.push_back(std::move(row));
        }
        engine.execute(&insert);
    }
}

void seedNativeContext(NativeEngine& engine,
                       int messageCount,
                       bool includeSensitive) {
    CreateContextStmt create;
    create.name = "bench_convo";
    engine.execute(&create);

    AliasTabStmt alias;
    alias.context = "bench_convo";
    alias.alias = "dog";
    alias.target = "convo about dog";
    engine.execute(&alias);

    for (int id = 1; id <= messageCount; ++id) {
        AppendMemoryStmt append;
        append.context = "bench_convo";
        append.messageId = static_cast<unsigned long long>(id);
        append.speaker = "user";
        if (includeSensitive && id % 7 == 0) {
            append.text = "Never share passwords or api key tokens.";
            append.tab = "constraints";
        } else {
            switch (id % 5) {
                case 0:
                    append.text = "My dog likes salmon.";
                    append.autoTab = true;
                    break;
                case 1:
                    append.text = "I like cat cafes.";
                    append.tab = "pet stuff";
                    break;
                case 2:
                    append.text =
                        "Decision: keep prompt views inside SkibidiQL.";
                    append.tab = "project roadmap";
                    break;
                case 3:
                    append.text =
                        "Debug this later: sqlite perf join misses.";
                    append.autoTab = true;
                    break;
                default:
                    append.text = "I live in Seattle.";
                    break;
            }
        }
        engine.execute(&append);
    }

    MergeTabsStmt merge;
    merge.context = "bench_convo";
    merge.fromTab = "pet stuff";
    merge.toTab = "dog";
    engine.execute(&merge);
}

std::string nativeQuery(const Options& options) {
    if (options.workload == "point") {
        return "slay id, name no-cap users only-if id = " +
               std::to_string(options.rows / 2) + ";";
    }
    if (options.workload == "scan") {
        return "slay headcount(*) no-cap users only-if active = 1;";
    }
    if (options.workload == "count_miss") {
        return "slay headcount(*) no-cap users only-if id > " +
               std::to_string(options.rows + 1000) + ";";
    }
    if (options.workload == "aggregate") {
        return "slay category, stack(score) no-cap users "
               "only-if active = 1 vibe-check category;";
    }
    if (options.workload == "context_schema") {
        return "show-context-schemas;";
    }
    if (options.workload == "context_spill") {
        return "spill-context bench_convo vibe-tab 'dog' "
               "only-if 'pet preferences' token-budget 512 receipts on;";
    }
    if (options.workload == "context_spill_acl") {
        return "spill-context bench_convo "
               "only-if 'constraints password policy' "
               "token-budget 512 receipts on;";
    }
    if (options.workload == "context_objects") {
        return "show-context-objects bench_convo;";
    }
    if (options.workload == "join_miss") {
        return "slay headcount(*) "
               "no-cap facts lowkey f "
               "link-up dimension_one lowkey d1 "
               "fr-fr f.d1_id = d1.id "
               "link-up dimension_two lowkey d2 "
               "fr-fr f.d2_id = d2.id;";
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
    if (options.workload == "count_miss") {
        return "SELECT COUNT(*) FROM users WHERE id > " +
               std::to_string(options.rows + 1000);
    }
    if (options.workload == "aggregate") {
        return "SELECT category, SUM(score) FROM users "
               "WHERE active = 1 GROUP BY category";
    }
    if (options.workload == "context_schema") {
        return "SELECT name, version, owner_agent, sensitivity, "
               "retention, storage, vectorized_fields, access_labels, "
               "indexed_fields, related_schemas FROM context_schemas";
    }
    if (options.workload == "context_spill") {
        return "SELECT type || ' ' || key || '=' || "
               "CASE WHEN instr(access_labels, "
               "'CONFIDENTIAL_CUSTOMER_DATA') > 0 "
               "THEN '[redacted:CONFIDENTIAL_CUSTOMER_DATA]' "
               "ELSE value END AS rendered "
               "FROM context_atoms "
               "WHERE status = 'active' AND tab = 'convo about dog' "
               "ORDER BY id DESC LIMIT 64";
    }
    if (options.workload == "context_spill_acl") {
        return "SELECT type || ' ' || key || '=' || "
               "CASE WHEN instr(access_labels, "
               "'CONFIDENTIAL_CUSTOMER_DATA') > 0 "
               "THEN '[redacted:CONFIDENTIAL_CUSTOMER_DATA]' "
               "ELSE value END AS rendered "
               "FROM context_atoms "
               "WHERE status = 'active' "
               "AND (key = 'user_constraint' "
               "OR instr(access_labels, "
               "'CONFIDENTIAL_CUSTOMER_DATA') > 0) "
               "ORDER BY id DESC LIMIT 64";
    }
    if (options.workload == "context_objects") {
        return "SELECT 'message_' || id, schema_name, schema_version, "
               "tab, 'active', access_labels, storage_route, '', "
               "CASE WHEN instr(access_labels, "
               "'CONFIDENTIAL_CUSTOMER_DATA') > 0 "
               "THEN '[redacted:CONFIDENTIAL_CUSTOMER_DATA]' "
               "ELSE text END "
               "FROM context_messages "
               "UNION ALL "
               "SELECT 'atom_' || id, schema_name, schema_version, "
               "tab, status, access_labels, "
               "'structured=catalog.contexts.atoms', source, "
               "type || ' ' || key || '=' || "
               "CASE WHEN instr(access_labels, "
               "'CONFIDENTIAL_CUSTOMER_DATA') > 0 "
               "THEN '[redacted:CONFIDENTIAL_CUSTOMER_DATA]' "
               "ELSE value END "
               "FROM context_atoms";
    }
    if (options.workload == "join_miss") {
        return "SELECT COUNT(*) "
               "FROM facts AS f "
               "JOIN dimension_one AS d1 ON f.d1_id = d1.id "
               "JOIN dimension_two AS d2 ON f.d2_id = d2.id";
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

void bindText(sqlite3_stmt* statement,
              int column,
              const std::string& value) {
    sqlite3_bind_text(
        statement, column, value.c_str(), -1, SQLITE_TRANSIENT);
}

void seedSqliteContextSchemas(sqlite3* database) {
    sqliteExecute(database,
        "CREATE TABLE context_schemas("
        "name TEXT, version TEXT, owner_agent TEXT, sensitivity TEXT, "
        "retention TEXT, storage TEXT, vectorized_fields TEXT, "
        "access_labels TEXT, indexed_fields TEXT, related_schemas TEXT);"
        "BEGIN");

    struct SchemaRow {
        const char* name;
        const char* version;
        const char* ownerAgent;
        const char* sensitivity;
        const char* retention;
        const char* storage;
        const char* vectorizedFields;
        const char* accessLabels;
        const char* indexedFields;
        const char* relatedSchemas;
    };
    const SchemaRow rows[] = {
        {"ConversationMessage", "v1", "system", "agent-internal",
         "conversation-lifetime", "structured+vector", "content",
         "AGENT_INTERNAL",
         "conversation_id,sender_id,sender_type,timestamp,tab,"
         "mentioned_entities,access_labels,schema_name,storage_route",
         "ContextAtom,TaskState,ToolInvocationLog"},
        {"ContextAtom", "v1", "system", "agent-internal",
         "conversation-lifetime", "structured", "",
         "AGENT_INTERNAL",
         "key,type,status,tab,source,invalidated_by,access_labels,"
         "schema_name",
         "ConversationMessage"},
        {"TaskState", "v1", "system", "agent-internal",
         "task-lifetime", "structured", "description",
         "AGENT_INTERNAL",
         "task_id,status,assigned_agent_id,last_update_timestamp",
         "ConversationMessage,ToolInvocationLog"},
        {"ToolInvocationLog", "v1", "system", "agent-internal",
         "audit-lifetime", "structured+blob", "summary",
         "AGENT_INTERNAL,TOOL_TRACE",
         "tool_id,conversation_id,message_id,status,timestamp",
         "ConversationMessage,TaskState"},
        {"UserProfile", "v1", "system", "confidential",
         "user-lifetime", "structured", "preferences",
         "CONFIDENTIAL_CUSTOMER_DATA",
         "user_id,tenant_id,access_labels",
         "ConversationMessage"},
    };

    sqlite3_stmt* insert = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "INSERT INTO context_schemas VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &insert, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    for (const auto& row : rows) {
        sqlite3_bind_text(insert, 1, row.name, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert, 2, row.version, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert, 3, row.ownerAgent, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert, 4, row.sensitivity, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert, 5, row.retention, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert, 6, row.storage, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert, 7, row.vectorizedFields, -1,
                          SQLITE_STATIC);
        sqlite3_bind_text(insert, 8, row.accessLabels, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert, 9, row.indexedFields, -1, SQLITE_STATIC);
        sqlite3_bind_text(insert, 10, row.relatedSchemas, -1,
                          SQLITE_STATIC);
        if (sqlite3_step(insert) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
    }
    sqlite3_finalize(insert);
    sqliteExecute(database, "COMMIT");
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

void seedSqliteJoin(sqlite3* database,
                    int rowCount,
                    bool missDimensionTwo = false) {
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
        sqlite3_bind_int(
            fact, 3,
            missDimensionTwo
                ? 1010 + (id % 10)
                : (id - 1) % 10 + 1);
        sqlite3_bind_int(fact, 4, id % 100);
        sqlite3_step(fact);
        sqlite3_reset(fact);
        sqlite3_clear_bindings(fact);
    }
    sqlite3_finalize(fact);
    sqliteExecute(database, "COMMIT");
}

void seedSqliteContext(sqlite3* database,
                       int messageCount,
                       bool includeSensitive) {
    sqliteExecute(database,
        "CREATE TABLE context_messages("
        "id INTEGER PRIMARY KEY, speaker TEXT, text TEXT, tab TEXT, "
        "schema_name TEXT, schema_version TEXT, access_labels TEXT, "
        "storage_route TEXT, mentioned_entities TEXT);"
        "CREATE TABLE context_atoms("
        "id INTEGER PRIMARY KEY, key TEXT, value TEXT, type TEXT, "
        "status TEXT, source TEXT, invalidated_by TEXT, tab TEXT, "
        "schema_name TEXT, schema_version TEXT, access_labels TEXT);"
        "BEGIN");

    sqlite3_stmt* message = nullptr;
    sqlite3_stmt* atom = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "INSERT INTO context_messages VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &message, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    if (sqlite3_prepare_v2(
            database,
            "INSERT INTO context_atoms VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &atom, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }

    int atomId = 1;
    for (int id = 1; id <= messageCount; ++id) {
        std::string text;
        std::string tab;
        std::string entities;
        std::string key;
        std::string value;
        std::string type;
        std::string labels = "AGENT_INTERNAL";

        if (includeSensitive && id % 7 == 0) {
            text = "Never share passwords or api key tokens.";
            tab = "constraints";
            entities = "storage-safety";
            key = "user_constraint";
            value = "never share passwords or api key tokens";
            type = "constraint";
            labels = "AGENT_INTERNAL,CONFIDENTIAL_CUSTOMER_DATA";
        } else {
            switch (id % 5) {
                case 0:
                    text = "My dog likes salmon.";
                    tab = "convo about dog";
                    entities = "dog";
                    key = "dog_preference";
                    value = "salmon";
                    type = "preference";
                    break;
                case 1:
                    text = "I like cat cafes.";
                    tab = "convo about dog";
                    entities = "dog";
                    key = "user_preference";
                    value = "cat cafes";
                    type = "preference";
                    break;
                case 2:
                    text = "Decision: keep prompt views inside SkibidiQL.";
                    tab = "project roadmap";
                    key = "decision";
                    value = "keep prompt views inside SkibidiQL";
                    type = "decision";
                    break;
                case 3:
                    text = "Debug this later: sqlite perf join misses.";
                    tab = "debugging sqlite perf";
                    entities = "sqlite,performance";
                    key = "debug_followup";
                    value = "sqlite perf join misses";
                    type = "debug";
                    break;
                default:
                    text = "I live in Seattle.";
                    tab = "main";
                    key = "user_location";
                    value = "Seattle";
                    type = "fact";
                    break;
            }
        }

        sqlite3_bind_int(message, 1, id);
        bindText(message, 2, "user");
        bindText(message, 3, text);
        bindText(message, 4, tab);
        bindText(message, 5, "ConversationMessage");
        bindText(message, 6, "v1");
        bindText(message, 7, labels);
        bindText(
            message, 8,
            "structured=catalog.contexts.messages; "
            "vector=ConversationMessage.content; blob=none");
        bindText(message, 9, entities);
        if (sqlite3_step(message) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
        sqlite3_reset(message);
        sqlite3_clear_bindings(message);

        sqlite3_bind_int(atom, 1, atomId++);
        bindText(atom, 2, key);
        bindText(atom, 3, value);
        bindText(atom, 4, type);
        bindText(atom, 5, "active");
        bindText(atom, 6, "message_" + std::to_string(id));
        bindText(atom, 7, "");
        bindText(atom, 8, tab);
        bindText(atom, 9, "ContextAtom");
        bindText(atom, 10, "v1");
        bindText(atom, 11, labels);
        if (sqlite3_step(atom) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
        sqlite3_reset(atom);
        sqlite3_clear_bindings(atom);
    }

    sqlite3_finalize(message);
    sqlite3_finalize(atom);
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
                  << ",\"buffer_memory_bytes\":"
                  << stats->residentPages * SlottedPage::PAGE_SIZE
                  << ",\"buffer_capacity_pages\":"
                  << stats->bufferCapacityPages
                  << ",\"buffer_page_reads\":"
                  << stats->bufferPageReads
                  << ",\"buffer_evictions\":"
                  << stats->bufferEvictions
                  << ",\"vector_batches\":" << stats->vectorBatches
                  << ",\"decoded_columns\":"
                  << stats->decodedColumns
                  << ",\"skipped_columns\":"
                  << stats->skippedColumns
                  << ",\"vector_nulls\":"
                  << stats->vectorNulls
                  << ",\"raw_point_queries\":"
                  << stats->rawPointQueries
                  << ",\"raw_point_hits\":"
                  << stats->rawPointHits
                  << ",\"direct_aggregate_queries\":"
                  << stats->directAggregateQueries
                  << ",\"value_count_queries\":"
                  << stats->valueCountQueries
                  << ",\"value_count_rows_answered\":"
                  << stats->valueCountRowsAnswered
                  << ",\"dense_group_aggregate_queries\":"
                  << stats->denseGroupAggregateQueries
                  << ",\"dense_group_aggregate_rows\":"
                  << stats->denseGroupAggregateRows
                  << ",\"raw_rows_scanned\":"
                  << stats->rawRowsScanned
                  << ",\"row_copies_avoided\":"
                  << stats->rowCopiesAvoided
                  << ",\"min_max_filters_checked\":"
                  << stats->minMaxFiltersChecked
                  << ",\"min_max_scans_skipped\":"
                  << stats->minMaxScansSkipped
                  << ",\"min_max_rows_skipped\":"
                  << stats->minMaxRowsSkipped
                  << ",\"min_max_statistics_built\":"
                  << stats->minMaxStatisticsBuilt
                  << ",\"min_max_build_rows\":"
                  << stats->minMaxBuildRows
                  << ",\"streaming_aggregate_queries\":"
                  << stats->streamingAggregateQueries
                  << ",\"streaming_aggregate_rows\":"
                  << stats->streamingAggregateRows
                  << ",\"rowid_seek_join_queries\":"
                  << stats->rowIdSeekJoinQueries
                  << ",\"rowid_seek_join_base_rows\":"
                  << stats->rowIdSeekJoinBaseRows
                  << ",\"rowid_seek_join_lookups\":"
                  << stats->rowIdSeekJoinLookups
                  << ",\"rowid_seek_join_misses\":"
                  << stats->rowIdSeekJoinMisses
                  << ",\"virtual_memory_scan_queries\":"
                  << stats->virtualMemoryScanQueries
                  << ",\"virtual_memory_rows_scanned\":"
                  << stats->virtualMemoryRowsScanned
                  << ",\"virtual_memory_rowid_reads\":"
                  << stats->virtualMemoryRowIdReads
                  << ",\"join_domain_filters_checked\":"
                  << stats->joinDomainFiltersChecked
                  << ",\"join_domain_scans_skipped\":"
                  << stats->joinDomainScansSkipped
                  << ",\"join_domain_rows_skipped\":"
                  << stats->joinDomainRowsSkipped
                  << ",\"join_plans_enumerated\":"
                  << stats->joinPlansEnumerated
                  << ",\"join_order_changes\":"
                  << stats->joinOrderChanges
                  << ",\"bloom_filter_builds\":"
                  << stats->bloomFilterBuilds
                  << ",\"bloom_filter_checks\":"
                  << stats->bloomFilterChecks
                  << ",\"bloom_filter_rejects\":"
                  << stats->bloomFilterRejects
                  << ",\"context_schema_queries\":"
                  << stats->contextSchemaQueries
                  << ",\"context_spill_queries\":"
                  << stats->contextSpillQueries
                  << ",\"context_object_queries\":"
                  << stats->contextObjectQueries
                  << ",\"context_cache_hits\":"
                  << stats->contextCacheHits
                  << ",\"context_cache_misses\":"
                  << stats->contextCacheMisses
                  << ",\"context_atoms_scored\":"
                  << stats->contextAtomsScored
                  << ",\"context_atoms_rendered\":"
                  << stats->contextAtomsRendered
                  << ",\"context_atoms_redacted\":"
                  << stats->contextAtomsRedacted
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
            if (isJoinWorkload(options.workload)) {
                seedNativeJoin(
                    engine, options.rows,
                    options.workload == "join_miss");
            } else if (isContextWorkload(options.workload)) {
                if (options.workload != "context_schema") {
                    seedNativeContext(
                        engine, options.rows,
                        options.workload == "context_spill_acl" ||
                        options.workload == "context_objects");
                }
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
                stats.estimatedMemoryBytes, &stats);
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
            if (isJoinWorkload(options.workload)) {
                seedSqliteJoin(
                    database, options.rows,
                    options.workload == "join_miss");
            } else if (options.workload == "context_schema") {
                seedSqliteContextSchemas(database);
            } else if (isContextWorkload(options.workload)) {
                seedSqliteContext(
                    database, options.rows,
                    options.workload == "context_spill_acl" ||
                    options.workload == "context_objects");
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
