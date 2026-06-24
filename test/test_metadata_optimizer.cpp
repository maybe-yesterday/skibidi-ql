#include "test_framework.h"

#include "codegen.h"
#include "lexer.h"
#include "metadata.h"
#include "optimizer.h"
#include "parser.h"

#include <string>

static Catalog makeCatalog() {
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

    ColumnMeta nickname;
    nickname.name = "nickname";
    nickname.type = "TEXT";

    users.columns.push_back(std::move(id));
    users.columns.push_back(std::move(name));
    users.columns.push_back(std::move(nickname));
    catalog.addTable(users);
    return catalog;
}

static std::pair<std::string, OptimizationReport> optimizeSql(
    const std::string& source,
    const Catalog& catalog) {
    Parser parser(Lexer(source).tokenize());
    auto statements = parser.parseAll();
    ASSERT_EQ(statements.size(), (size_t)1);

    OptimizationReport report;
    Optimizer optimizer(false, &catalog);
    auto optimized =
        optimizer.optimize(std::move(statements.front()), report);
    return {CodeGen().generate(optimized.get()), std::move(report)};
}

static bool hasNote(const OptimizationReport& report,
                    const std::string& needle) {
    for (const auto& note : report.notes) {
        if (note.find(needle) != std::string::npos) return true;
    }
    return false;
}

TEST(non_null_count_rewrites_to_count_star) {
    Catalog catalog = makeCatalog();
    auto result =
        optimizeSql("slay headcount(name) no-cap users;", catalog);
    ASSERT_CONTAINS(result.first, "COUNT(*)");
    ASSERT_TRUE(hasNote(result.second, "COUNT(name)"));
}

TEST(nullable_count_is_not_rewritten) {
    Catalog catalog = makeCatalog();
    auto result =
        optimizeSql("slay headcount(nickname) no-cap users;", catalog);
    ASSERT_CONTAINS(result.first, "COUNT(nickname)");
}

TEST(primary_key_projection_removes_distinct) {
    Catalog catalog = makeCatalog();
    auto result =
        optimizeSql("slay unique-fr id, name no-cap users;", catalog);
    ASSERT_FALSE(result.first.find("DISTINCT") != std::string::npos);
    ASSERT_TRUE(hasNote(result.second, "redundant DISTINCT"));
}

TEST(distinct_remains_without_primary_key_projection) {
    Catalog catalog = makeCatalog();
    auto result =
        optimizeSql("slay unique-fr name no-cap users;", catalog);
    ASSERT_CONTAINS(result.first, "SELECT DISTINCT");
}

TEST(nullable_text_primary_key_does_not_remove_distinct) {
    Catalog catalog;
    TableMeta legacy;
    legacy.name = "legacy";
    ColumnMeta key;
    key.name = "key";
    key.type = "TEXT";
    key.primary_key = true;
    legacy.columns.push_back(std::move(key));
    catalog.addTable(legacy);

    auto result =
        optimizeSql("slay unique-fr key no-cap legacy;", catalog);
    ASSERT_CONTAINS(result.first, "SELECT DISTINCT");
}

TEST(primary_key_lookup_adds_limit_and_removes_order) {
    Catalog catalog = makeCatalog();
    auto result = optimizeSql(
        "slay id, name no-cap users only-if id = 7 "
        "hits-different name down-bad;",
        catalog);
    ASSERT_CONTAINS(result.first, "LIMIT 1");
    ASSERT_FALSE(result.first.find("ORDER BY") != std::string::npos);
    ASSERT_TRUE(hasNote(result.second, "primary-key point lookup"));
}

TEST(non_key_lookup_keeps_order_and_no_limit) {
    Catalog catalog = makeCatalog();
    auto result = optimizeSql(
        "slay id, name no-cap users only-if name = 'A' "
        "hits-different name;",
        catalog);
    ASSERT_CONTAINS(result.first, "ORDER BY");
    ASSERT_FALSE(result.first.find("LIMIT 1") != std::string::npos);
}

TEST(metadata_rewrites_are_disabled_for_joins) {
    Catalog catalog = makeCatalog();
    TableMeta orders;
    orders.name = "orders";
    ColumnMeta userId;
    userId.name = "user_id";
    userId.type = "INTEGER";
    userId.not_null = true;
    orders.columns.push_back(std::move(userId));
    catalog.addTable(orders);

    auto result = optimizeSql(
        "slay headcount(name) no-cap users lowkey u "
        "link-up orders lowkey o fr-fr u.id = o.user_id;",
        catalog);
    ASSERT_CONTAINS(result.first, "COUNT(name)");
}

int main() {
    return run_all_tests("Metadata optimizer");
}
