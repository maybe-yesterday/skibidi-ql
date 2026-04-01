// Tests for CodeGen::generate()
// Builds ASTs manually or via the Lexer+Parser pipeline and verifies
// the generated SQL string output.
#include "test_framework.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "ast.h"
#include <memory>
#include <string>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Full pipeline: SkibidiQL source → SQL string
static std::string toSQL(const std::string& src) {
    auto toks = Lexer(src).tokenize();
    Parser p(std::move(toks));
    auto stmts = p.parseAll();
    ASSERT_EQ(stmts.size(), (size_t)1);
    CodeGen cg;
    return cg.generate(stmts[0].get());
}

// -----------------------------------------------------------------------
// generate() null input
// -----------------------------------------------------------------------
TEST(generate_null_returns_empty) {
    CodeGen cg;
    ASSERT_EQ(cg.generate(nullptr), std::string(""));
}

// -----------------------------------------------------------------------
// SELECT – basic
// -----------------------------------------------------------------------
TEST(generate_select_star) {
    std::string sql = toSQL("slay * no-cap users");
    ASSERT_CONTAINS(sql, "SELECT");
    ASSERT_CONTAINS(sql, "*");
    ASSERT_CONTAINS(sql, "FROM");
    ASSERT_CONTAINS(sql, "users");
}

TEST(generate_select_distinct) {
    std::string sql = toSQL("slay unique-fr * no-cap users");
    ASSERT_CONTAINS(sql, "SELECT DISTINCT");
}

TEST(generate_select_specific_columns) {
    std::string sql = toSQL("slay id, name no-cap users");
    ASSERT_CONTAINS(sql, "id");
    ASSERT_CONTAINS(sql, "name");
    ASSERT_CONTAINS(sql, "FROM users");
}

TEST(generate_select_column_alias) {
    std::string sql = toSQL("slay id lowkey user_id no-cap users");
    ASSERT_CONTAINS(sql, "id AS user_id");
}

TEST(generate_select_table_alias) {
    std::string sql = toSQL("slay * no-cap users lowkey u");
    ASSERT_CONTAINS(sql, "users AS u");
}

// -----------------------------------------------------------------------
// SELECT – WHERE clause
// -----------------------------------------------------------------------
TEST(generate_select_where_eq) {
    std::string sql = toSQL("slay * no-cap users only-if id = 1");
    ASSERT_CONTAINS(sql, "WHERE");
    ASSERT_CONTAINS(sql, "= 1");
}

TEST(generate_select_where_ne) {
    std::string sql = toSQL("slay * no-cap users only-if status != 0");
    ASSERT_CONTAINS(sql, "!= 0");
}

TEST(generate_select_where_lt_gt) {
    std::string sql = toSQL("slay * no-cap items only-if price < 100");
    ASSERT_CONTAINS(sql, "< 100");
    std::string sql2 = toSQL("slay * no-cap items only-if price > 0");
    ASSERT_CONTAINS(sql2, "> 0");
}

TEST(generate_select_where_lte_gte) {
    std::string sql = toSQL("slay * no-cap items only-if price <= 50");
    ASSERT_CONTAINS(sql, "<= 50");
    std::string sql2 = toSQL("slay * no-cap items only-if qty >= 1");
    ASSERT_CONTAINS(sql2, ">= 1");
}

TEST(generate_select_where_and) {
    std::string sql = toSQL("slay * no-cap users only-if id > 0 plus active = 1");
    ASSERT_CONTAINS(sql, "AND");
}

TEST(generate_select_where_or) {
    std::string sql = toSQL("slay * no-cap users only-if id = 1 or-nah id = 2");
    ASSERT_CONTAINS(sql, "OR");
}

TEST(generate_select_where_not) {
    std::string sql = toSQL("slay * no-cap users only-if no-cap-not active = 1");
    ASSERT_CONTAINS(sql, "NOT");
}

TEST(generate_select_where_string_value) {
    std::string sql = toSQL("slay * no-cap users only-if name = 'Alice'");
    ASSERT_CONTAINS(sql, "'Alice'");
}

TEST(generate_select_where_null) {
    std::string sql = toSQL("slay * no-cap users only-if name = ghosted");
    ASSERT_CONTAINS(sql, "NULL");
}

// -----------------------------------------------------------------------
// SELECT – GROUP BY, HAVING
// -----------------------------------------------------------------------
TEST(generate_select_group_by) {
    std::string sql = toSQL("slay dept, headcount(*) no-cap employees vibe-check dept");
    ASSERT_CONTAINS(sql, "GROUP BY");
    ASSERT_CONTAINS(sql, "dept");
}

TEST(generate_select_having) {
    std::string sql = toSQL(
        "slay dept no-cap employees vibe-check dept bussin-only headcount(*) > 5");
    ASSERT_CONTAINS(sql, "HAVING");
    ASSERT_CONTAINS(sql, "> 5");
}

// -----------------------------------------------------------------------
// SELECT – ORDER BY
// -----------------------------------------------------------------------
TEST(generate_select_order_by_asc) {
    std::string sql = toSQL("slay * no-cap users hits-different name up-only");
    ASSERT_CONTAINS(sql, "ORDER BY");
    ASSERT_CONTAINS(sql, "ASC");
}

TEST(generate_select_order_by_desc) {
    std::string sql = toSQL("slay * no-cap users hits-different id down-bad");
    ASSERT_CONTAINS(sql, "DESC");
}

// -----------------------------------------------------------------------
// SELECT – LIMIT and OFFSET
// -----------------------------------------------------------------------
TEST(generate_select_limit) {
    std::string sql = toSQL("slay * no-cap users cap-at 10");
    ASSERT_CONTAINS(sql, "LIMIT 10");
}

TEST(generate_select_offset) {
    std::string sql = toSQL("slay * no-cap users cap-at 10 skip 5");
    ASSERT_CONTAINS(sql, "LIMIT 10");
    ASSERT_CONTAINS(sql, "OFFSET 5");
}

// -----------------------------------------------------------------------
// SELECT – JOINs
// -----------------------------------------------------------------------
TEST(generate_select_inner_join) {
    std::string sql = toSQL(
        "slay * no-cap users lowkey u mid-link-up orders lowkey o fr-fr u.id = o.user_id");
    ASSERT_CONTAINS(sql, "JOIN");
    ASSERT_CONTAINS(sql, "orders");
    ASSERT_CONTAINS(sql, "ON");
    ASSERT_CONTAINS(sql, "u.id");
    ASSERT_CONTAINS(sql, "o.user_id");
}

TEST(generate_select_left_join) {
    std::string sql = toSQL(
        "slay * no-cap users lowkey u left-link-up orders lowkey o fr-fr u.id = o.user_id");
    ASSERT_CONTAINS(sql, "LEFT JOIN");
}

// -----------------------------------------------------------------------
// SELECT – aggregate function names mapped correctly
// -----------------------------------------------------------------------
TEST(generate_headcount_star_maps_to_COUNT) {
    std::string sql = toSQL("slay headcount(*) no-cap users");
    ASSERT_CONTAINS(sql, "COUNT(*)");
}

TEST(generate_headcount_distinct_maps_to_COUNT_DISTINCT) {
    std::string sql = toSQL("slay headcount(unique-fr id) no-cap users");
    ASSERT_CONTAINS(sql, "COUNT(DISTINCT id)");
}

TEST(generate_stack_maps_to_SUM) {
    std::string sql = toSQL("slay stack(amount) no-cap orders");
    ASSERT_CONTAINS(sql, "SUM(amount)");
}

TEST(generate_mid_maps_to_AVG) {
    std::string sql = toSQL("slay mid(score) no-cap results");
    ASSERT_CONTAINS(sql, "AVG(score)");
}

TEST(generate_goat_maps_to_MAX) {
    std::string sql = toSQL("slay goat(salary) no-cap employees");
    ASSERT_CONTAINS(sql, "MAX(salary)");
}

TEST(generate_L_maps_to_MIN) {
    std::string sql = toSQL("slay L(salary) no-cap employees");
    ASSERT_CONTAINS(sql, "MIN(salary)");
}

// -----------------------------------------------------------------------
// SELECT – expression codegen
// -----------------------------------------------------------------------
TEST(generate_arithmetic_plus) {
    std::string sql = toSQL("slay price + tax no-cap items");
    ASSERT_CONTAINS(sql, "price + tax");
}

TEST(generate_arithmetic_multiply) {
    std::string sql = toSQL("slay qty * price no-cap items");
    ASSERT_CONTAINS(sql, "qty * price");
}

TEST(generate_string_concat) {
    std::string sql = toSQL("slay first_name || last_name no-cap users");
    ASSERT_CONTAINS(sql, "||");
}

TEST(generate_unary_minus) {
    std::string sql = toSQL("slay -amount no-cap t");
    ASSERT_CONTAINS(sql, "-amount");
}

TEST(generate_literal_int) {
    std::string sql = toSQL("slay 42 no-cap t");
    ASSERT_CONTAINS(sql, "42");
}

TEST(generate_literal_float) {
    std::string sql = toSQL("slay 3.14 no-cap t");
    ASSERT_CONTAINS(sql, "3.14");
}

TEST(generate_literal_string_quoted) {
    std::string sql = toSQL("slay 'hello' no-cap t");
    ASSERT_CONTAINS(sql, "'hello'");
}

TEST(generate_literal_null) {
    std::string sql = toSQL("slay ghosted no-cap t");
    ASSERT_CONTAINS(sql, "NULL");
}

TEST(generate_literal_bool_true_as_1) {
    // true/false parsed as identifiers by parser (not boolean literals),
    // so test via the AST directly
    auto boolLit = Literal::makeBool(true);
    auto wc = std::make_unique<Wildcard>();
    auto sel = std::make_unique<SelectStmt>();
    sel->columns.push_back(std::move(boolLit));
    sel->fromTable = "t";

    CodeGen cg;
    std::string sql = cg.generate(sel.get());
    ASSERT_CONTAINS(sql, "1");  // true → 1
}

TEST(generate_literal_bool_false_as_0) {
    auto boolLit = Literal::makeBool(false);
    auto sel = std::make_unique<SelectStmt>();
    sel->columns.push_back(std::move(boolLit));
    sel->fromTable = "t";

    CodeGen cg;
    std::string sql = cg.generate(sel.get());
    ASSERT_CONTAINS(sql, "0");  // false → 0
}

TEST(generate_wildcard_bare) {
    auto wc = std::make_unique<Wildcard>();
    auto sel = std::make_unique<SelectStmt>();
    sel->columns.push_back(std::move(wc));
    sel->fromTable = "t";
    CodeGen cg;
    ASSERT_CONTAINS(cg.generate(sel.get()), "*");
}

TEST(generate_wildcard_qualified) {
    auto wc = std::make_unique<Wildcard>();
    wc->table = "u";
    auto sel = std::make_unique<SelectStmt>();
    sel->columns.push_back(std::move(wc));
    sel->fromTable = "users";
    sel->fromAlias = "u";
    CodeGen cg;
    ASSERT_CONTAINS(cg.generate(sel.get()), "u.*");
}

// -----------------------------------------------------------------------
// SELECT – window function (era → RANK() OVER)
// -----------------------------------------------------------------------
TEST(generate_era_window_function) {
    std::string sql = toSQL("slay era hits-different salary down-bad no-cap employees");
    ASSERT_CONTAINS(sql, "RANK() OVER (");
    ASSERT_CONTAINS(sql, "ORDER BY");
    ASSERT_CONTAINS(sql, "DESC");
}

TEST(generate_era_with_partition) {
    std::string sql = toSQL("slay era split-by dept hits-different salary no-cap employees");
    ASSERT_CONTAINS(sql, "PARTITION BY dept");
    ASSERT_CONTAINS(sql, "ORDER BY salary");
}

// -----------------------------------------------------------------------
// SELECT – special analytics: biggest-W (ARGMAX)
// -----------------------------------------------------------------------
TEST(generate_biggest_W_produces_order_desc_limit1) {
    std::string sql = toSQL("slay biggest-W(salary) no-cap employees");
    ASSERT_CONTAINS(sql, "ORDER BY salary DESC");
    ASSERT_CONTAINS(sql, "LIMIT 1");
}

TEST(generate_biggest_L_produces_order_asc_limit1) {
    std::string sql = toSQL("slay biggest-L(salary) no-cap employees");
    ASSERT_CONTAINS(sql, "ORDER BY salary ASC");
    ASSERT_CONTAINS(sql, "LIMIT 1");
}

// -----------------------------------------------------------------------
// SELECT – special analytics: mid-fr (MEDIAN CTE)
// -----------------------------------------------------------------------
TEST(generate_median_uses_cte) {
    std::string sql = toSQL("slay mid-fr(salary) no-cap employees");
    ASSERT_CONTAINS(sql, "WITH");
    ASSERT_CONTAINS(sql, "AVG");
    ASSERT_CONTAINS(sql, "ROW_NUMBER() OVER");
}

TEST(generate_median_alias_preserved) {
    std::string sql = toSQL("slay mid-fr(salary) lowkey med_sal no-cap employees");
    ASSERT_CONTAINS(sql, "AS med_sal");
}

// -----------------------------------------------------------------------
// SELECT – special analytics: percent-check (PERCENTILE CTE)
// -----------------------------------------------------------------------
TEST(generate_percentile_uses_cte) {
    std::string sql = toSQL("slay percent-check(salary, 90) no-cap employees");
    ASSERT_CONTAINS(sql, "WITH");
    ASSERT_CONTAINS(sql, "CEIL");
    ASSERT_CONTAINS(sql, "ROW_NUMBER() OVER");
}

// -----------------------------------------------------------------------
// INSERT
// -----------------------------------------------------------------------
TEST(generate_insert_with_columns) {
    std::string sql = toSQL("yeet-into users (id, name) drip (1, 'Alice')");
    ASSERT_CONTAINS(sql, "INSERT INTO users");
    ASSERT_CONTAINS(sql, "(id, name)");
    ASSERT_CONTAINS(sql, "VALUES");
    ASSERT_CONTAINS(sql, "(1, 'Alice')");
}

TEST(generate_insert_without_columns) {
    std::string sql = toSQL("yeet-into users drip (1, 'Alice')");
    ASSERT_CONTAINS(sql, "INSERT INTO users");
    // No column list in the output
    ASSERT_CONTAINS(sql, "VALUES");
    ASSERT_CONTAINS(sql, "(1, 'Alice')");
}

TEST(generate_insert_multiple_rows) {
    std::string sql = toSQL("yeet-into users (id, name) drip (1, 'Alice'), (2, 'Bob')");
    ASSERT_CONTAINS(sql, "(1, 'Alice')");
    ASSERT_CONTAINS(sql, "(2, 'Bob')");
}

TEST(generate_insert_null_value) {
    std::string sql = toSQL("yeet-into users (id, name) drip (1, ghosted)");
    ASSERT_CONTAINS(sql, "NULL");
}

// -----------------------------------------------------------------------
// UPDATE
// -----------------------------------------------------------------------
TEST(generate_update_basic) {
    std::string sql = toSQL("glow-up users be-like name = 'Alice' only-if id = 1");
    ASSERT_CONTAINS(sql, "UPDATE users SET");
    ASSERT_CONTAINS(sql, "name = 'Alice'");
    ASSERT_CONTAINS(sql, "WHERE");
}

TEST(generate_update_without_where) {
    std::string sql = toSQL("glow-up users be-like active = 0");
    ASSERT_CONTAINS(sql, "UPDATE users SET");
    ASSERT_CONTAINS(sql, "active = 0");
    // No WHERE clause
    ASSERT_FALSE(sql.find("WHERE") != std::string::npos);
}

TEST(generate_update_multiple_assignments) {
    std::string sql = toSQL("glow-up users be-like name = 'Bob', active = 1");
    ASSERT_CONTAINS(sql, "name = 'Bob'");
    ASSERT_CONTAINS(sql, "active = 1");
}

// -----------------------------------------------------------------------
// DELETE
// -----------------------------------------------------------------------
TEST(generate_delete_with_where) {
    std::string sql = toSQL("ratio users only-if id = 1");
    ASSERT_CONTAINS(sql, "DELETE FROM users");
    ASSERT_CONTAINS(sql, "WHERE");
}

TEST(generate_delete_without_where) {
    std::string sql = toSQL("ratio temp_table");
    ASSERT_CONTAINS(sql, "DELETE FROM temp_table");
    ASSERT_FALSE(sql.find("WHERE") != std::string::npos);
}

// -----------------------------------------------------------------------
// CREATE TABLE
// -----------------------------------------------------------------------
TEST(generate_create_table_basic) {
    std::string sql = toSQL("manifest users (id INTEGER, name TEXT)");
    ASSERT_CONTAINS(sql, "CREATE TABLE users");
    ASSERT_CONTAINS(sql, "id INTEGER");
    ASSERT_CONTAINS(sql, "name TEXT");
}

TEST(generate_create_table_primary_key) {
    std::string sql = toSQL("manifest users (id INTEGER main-character)");
    ASSERT_CONTAINS(sql, "PRIMARY KEY");
}

TEST(generate_create_table_not_null) {
    std::string sql = toSQL("manifest users (id INTEGER, name TEXT no-cap-not ghosted)");
    ASSERT_CONTAINS(sql, "NOT NULL");
}

TEST(generate_create_table_foreign_key) {
    std::string sql = toSQL(
        "manifest orders (id INTEGER, user_id INTEGER side-character references users(id))");
    ASSERT_CONTAINS(sql, "REFERENCES users(id)");
}

// -----------------------------------------------------------------------
// DROP TABLE
// -----------------------------------------------------------------------
TEST(generate_drop_table) {
    std::string sql = toSQL("rizz-down users");
    ASSERT_CONTAINS(sql, "DROP TABLE users");
}

TEST(generate_drop_table_name_correct) {
    std::string sql = toSQL("rizz-down order_history");
    ASSERT_CONTAINS(sql, "DROP TABLE order_history");
}

// -----------------------------------------------------------------------
// String quoting in generated SQL
// -----------------------------------------------------------------------
TEST(generate_string_with_single_quote_doubled) {
    // Build AST manually: INSERT with a string containing a quote
    auto ins = std::make_unique<InsertStmt>();
    ins->table = "t";
    ins->columns.push_back("name");
    std::vector<std::unique_ptr<ASTNode>> row;
    row.push_back(Literal::makeString("it's here"));
    ins->valueRows.push_back(std::move(row));

    CodeGen cg;
    std::string sql = cg.generate(ins.get());
    // The single quote should be doubled in the output
    ASSERT_CONTAINS(sql, "it''s here");
}

int main() {
    return run_all_tests("CodeGen");
}
