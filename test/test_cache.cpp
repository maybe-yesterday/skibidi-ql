#include "test_framework.h"

#include "cache.h"
#include "compiler.h"
#include "metadata.h"

#include <string>

static CachedCompilation cachedSql(const std::string& sql) {
    CachedCompilation value;
    CachedStatement statement;
    statement.sql = sql;
    value.statements.push_back(std::move(statement));
    return value;
}

static TableMeta usersMetadata() {
    TableMeta table;
    table.name = "users";

    ColumnMeta id;
    id.name = "id";
    id.type = "INTEGER";
    id.primary_key = true;

    ColumnMeta name;
    name.name = "name";
    name.type = "TEXT";
    name.not_null = true;

    table.columns.push_back(std::move(id));
    table.columns.push_back(std::move(name));
    return table;
}

TEST(cache_records_hits_and_misses) {
    CompilationCache cache(4, 4096);
    CachedCompilationHandle value;

    ASSERT_FALSE(cache.get("missing", 1, value));
    cache.put("query", 1, cachedSql("SELECT 1"));
    ASSERT_TRUE(cache.get("query", 1, value));
    ASSERT_EQ(value->statements[0].sql, std::string("SELECT 1"));

    const CacheStats stats = cache.stats();
    ASSERT_EQ(stats.hits, (size_t)1);
    ASSERT_EQ(stats.misses, (size_t)1);
    ASSERT_EQ(stats.entries, (size_t)1);
}

TEST(cache_evicts_least_recently_used_entry) {
    CompilationCache cache(2, 4096);
    CachedCompilationHandle value;
    cache.put("a", 1, cachedSql("SELECT 'a'"));
    cache.put("b", 1, cachedSql("SELECT 'b'"));
    ASSERT_TRUE(cache.get("a", 1, value));
    cache.put("c", 1, cachedSql("SELECT 'c'"));

    ASSERT_FALSE(cache.get("b", 1, value));
    ASSERT_TRUE(cache.get("a", 1, value));
    ASSERT_TRUE(cache.get("c", 1, value));
    ASSERT_EQ(cache.stats().evictions, (size_t)1);
}

TEST(cache_rejects_entry_larger_than_byte_budget) {
    CompilationCache cache(4, 16);
    cache.put("large-key", 1, cachedSql(std::string(128, 'x')));
    ASSERT_EQ(cache.stats().entries, (size_t)0);
    ASSERT_EQ(cache.stats().bytes, (size_t)0);
}

TEST(cache_clear_keeps_statistics_but_removes_entries) {
    CompilationCache cache(4, 4096);
    CachedCompilationHandle value;
    cache.put("a", 1, cachedSql("SELECT 1"));
    ASSERT_TRUE(cache.get("a", 1, value));
    cache.clear();

    ASSERT_EQ(cache.stats().entries, (size_t)0);
    ASSERT_EQ(cache.stats().hits, (size_t)1);
}

TEST(query_compiler_returns_cached_sql_on_second_compile) {
    Catalog catalog;
    catalog.addTable(usersMetadata());
    QueryCompiler compiler;
    const std::string source =
        "slay id, name no-cap users only-if id = 1;";

    auto first = compiler.compile(source, catalog);
    auto second = compiler.compile(source, catalog);

    ASSERT_FALSE(first.cacheHit);
    ASSERT_TRUE(second.cacheHit);
    ASSERT_EQ(first.statements.size(), (size_t)1);
    ASSERT_EQ(second.statements.size(), (size_t)1);
    ASSERT_EQ(first.statements[0].sql, second.statements[0].sql);
    ASSERT_TRUE(first.statements[0].ast != nullptr);
    ASSERT_TRUE(second.statements[0].ast == nullptr);
}

TEST(query_compiler_fast_path_reuses_cached_storage) {
    Catalog catalog;
    catalog.addTable(usersMetadata());
    QueryCompiler compiler;
    const std::string source =
        "slay id, name no-cap users only-if id = 1;";

    auto first = compiler.compileFast(source, catalog);
    auto second = compiler.compileFast(source, catalog);

    ASSERT_FALSE(first.cacheHit);
    ASSERT_TRUE(second.cacheHit);
    ASSERT_TRUE(first.output != nullptr);
    ASSERT_TRUE(first.output == second.output);
    ASSERT_EQ(second.output->statements[0].sql,
              first.output->statements[0].sql);
    ASSERT_TRUE(second.detailed.statements.empty());
}

TEST(query_compiler_cache_key_tracks_schema_changes) {
    Catalog catalog;
    catalog.addTable(usersMetadata());
    QueryCompiler compiler;
    const std::string source = "slay * no-cap users;";

    (void)compiler.compile(source, catalog);
    ASSERT_TRUE(compiler.compile(source, catalog).cacheHit);

    TableMeta other;
    other.name = "other";
    catalog.addTable(other);
    ASSERT_FALSE(compiler.compile(source, catalog).cacheHit);
}

TEST(query_compiler_distinguishes_catalogs_with_same_revision) {
    Catalog firstCatalog;
    firstCatalog.addTable(usersMetadata());

    Catalog secondCatalog;
    TableMeta users = usersMetadata();
    users.columns[0].primary_key = false;
    secondCatalog.addTable(users);
    ASSERT_EQ(firstCatalog.revision(), secondCatalog.revision());

    QueryCompiler compiler;
    const std::string source =
        "slay id no-cap users only-if id = 1;";
    auto first = compiler.compile(source, firstCatalog);
    auto second = compiler.compile(source, secondCatalog);

    ASSERT_CONTAINS(first.statements[0].sql, "LIMIT 1");
    ASSERT_FALSE(second.cacheHit);
    ASSERT_FALSE(second.statements[0].sql.find("LIMIT 1") != std::string::npos);
}

TEST(schema_changing_queries_are_not_cached) {
    Catalog catalog;
    QueryCompiler compiler;
    const std::string source = "manifest users (id INTEGER);";

    auto first = compiler.compile(source, catalog);
    auto second = compiler.compile(source, catalog);
    ASSERT_FALSE(first.cacheable);
    ASSERT_FALSE(second.cacheHit);
    ASSERT_EQ(compiler.cacheStats().entries, (size_t)0);
}

int main() {
    return run_all_tests("Cache");
}
