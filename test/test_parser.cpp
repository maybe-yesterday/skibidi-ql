// Tests for Parser::parseAll() and Parser::parseStatement()
// Uses the Lexer → Parser pipeline since Parser takes a token stream.
// Covers: SELECT, INSERT, UPDATE, DELETE, CREATE TABLE, DROP TABLE,
//         expression parsing, error cases, multiple statements.
#include "test_framework.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include <vector>
#include <memory>
#include <stdexcept>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
static std::vector<std::unique_ptr<ASTNode>> parse(const std::string& src) {
    auto toks = Lexer(src).tokenize();
    Parser p(std::move(toks));
    return p.parseAll();
}

static std::unique_ptr<ASTNode> parseOne(const std::string& src) {
    auto stmts = parse(src);
    ASSERT_EQ(stmts.size(), (size_t)1);
    return std::move(stmts[0]);
}

static SelectStmt* asSelect(const std::unique_ptr<ASTNode>& node) {
    auto* s = dynamic_cast<SelectStmt*>(node.get());
    ASSERT_TRUE(s != nullptr);
    return s;
}

static InsertStmt* asInsert(const std::unique_ptr<ASTNode>& node) {
    auto* s = dynamic_cast<InsertStmt*>(node.get());
    ASSERT_TRUE(s != nullptr);
    return s;
}

static UpdateStmt* asUpdate(const std::unique_ptr<ASTNode>& node) {
    auto* s = dynamic_cast<UpdateStmt*>(node.get());
    ASSERT_TRUE(s != nullptr);
    return s;
}

static DeleteStmt* asDelete(const std::unique_ptr<ASTNode>& node) {
    auto* s = dynamic_cast<DeleteStmt*>(node.get());
    ASSERT_TRUE(s != nullptr);
    return s;
}

static CreateStmt* asCreate(const std::unique_ptr<ASTNode>& node) {
    auto* s = dynamic_cast<CreateStmt*>(node.get());
    ASSERT_TRUE(s != nullptr);
    return s;
}

static DropStmt* asDrop(const std::unique_ptr<ASTNode>& node) {
    auto* s = dynamic_cast<DropStmt*>(node.get());
    ASSERT_TRUE(s != nullptr);
    return s;
}

// -----------------------------------------------------------------------
// parseAll() – basic
// -----------------------------------------------------------------------
TEST(parseAll_empty_returns_no_statements) {
    auto stmts = parse("");
    ASSERT_EQ(stmts.size(), (size_t)0);
}

TEST(parseAll_single_statement) {
    auto stmts = parse("slay * no-cap users");
    ASSERT_EQ(stmts.size(), (size_t)1);
}

TEST(parseAll_multiple_statements_semicolon_separated) {
    auto stmts = parse("slay * no-cap a ; slay * no-cap b");
    ASSERT_EQ(stmts.size(), (size_t)2);
}

TEST(parseAll_trailing_semicolon_ignored) {
    auto stmts = parse("slay * no-cap users ;");
    ASSERT_EQ(stmts.size(), (size_t)1);
}

TEST(parseAll_multiple_semicolons_between_statements) {
    auto stmts = parse("slay * no-cap a ;; slay * no-cap b");
    ASSERT_EQ(stmts.size(), (size_t)2);
}

// -----------------------------------------------------------------------
// parseStatement() – unknown token throws ParseError
// -----------------------------------------------------------------------
TEST(parseStatement_unknown_token_throws) {
    auto toks = Lexer("42").tokenize();
    Parser p(std::move(toks));
    ASSERT_THROW(p.parseStatement(), ParseError);
}

// -----------------------------------------------------------------------
// SELECT – basic forms
// -----------------------------------------------------------------------
TEST(select_star_from_table) {
    auto stmt = parseOne("slay * no-cap users");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->fromTable, std::string("users"));
    ASSERT_EQ(s->columns.size(), (size_t)1);
    ASSERT_TRUE(dynamic_cast<Wildcard*>(s->columns[0].get()) != nullptr);
    ASSERT_FALSE(s->distinct);
}

TEST(select_specific_columns) {
    auto stmt = parseOne("slay id, name no-cap users");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->columns.size(), (size_t)2);
    auto* c0 = dynamic_cast<ColumnRef*>(s->columns[0].get());
    ASSERT_TRUE(c0 != nullptr);
    ASSERT_EQ(c0->column, std::string("id"));
    auto* c1 = dynamic_cast<ColumnRef*>(s->columns[1].get());
    ASSERT_TRUE(c1 != nullptr);
    ASSERT_EQ(c1->column, std::string("name"));
}

TEST(select_distinct) {
    auto stmt = parseOne("slay unique-fr * no-cap users");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->distinct);
}

TEST(select_table_alias) {
    auto stmt = parseOne("slay * no-cap users lowkey u");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->fromTable, std::string("users"));
    ASSERT_EQ(s->fromAlias, std::string("u"));
}

TEST(select_column_alias) {
    auto stmt = parseOne("slay id lowkey user_id no-cap users");
    auto* s = asSelect(stmt);
    auto* cr = dynamic_cast<ColumnRef*>(s->columns[0].get());
    ASSERT_TRUE(cr != nullptr);
    ASSERT_EQ(cr->alias, std::string("user_id"));
}

// -----------------------------------------------------------------------
// SELECT – WHERE clause
// -----------------------------------------------------------------------
TEST(select_with_where_eq) {
    auto stmt = parseOne("slay * no-cap users only-if id = 1");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->where != nullptr);
    auto* w = dynamic_cast<BinaryOp*>(s->where.get());
    ASSERT_TRUE(w != nullptr);
    ASSERT_EQ(w->op, std::string("="));
}

TEST(select_with_where_and) {
    auto stmt = parseOne("slay * no-cap users only-if id = 1 plus active = 1");
    auto* s = asSelect(stmt);
    auto* w = dynamic_cast<BinaryOp*>(s->where.get());
    ASSERT_TRUE(w != nullptr);
    ASSERT_EQ(w->op, std::string("AND"));
}

TEST(select_with_where_or) {
    auto stmt = parseOne("slay * no-cap users only-if id = 1 or-nah id = 2");
    auto* s = asSelect(stmt);
    auto* w = dynamic_cast<BinaryOp*>(s->where.get());
    ASSERT_TRUE(w != nullptr);
    ASSERT_EQ(w->op, std::string("OR"));
}

TEST(select_with_where_not) {
    auto stmt = parseOne("slay * no-cap users only-if no-cap-not active = 1");
    auto* s = asSelect(stmt);
    auto* w = dynamic_cast<UnaryOp*>(s->where.get());
    ASSERT_TRUE(w != nullptr);
    ASSERT_EQ(w->op, std::string("NOT"));
}

TEST(select_where_string_literal) {
    auto stmt = parseOne("slay * no-cap users only-if name = 'Alice'");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->where != nullptr);
    auto* bo = dynamic_cast<BinaryOp*>(s->where.get());
    ASSERT_TRUE(bo != nullptr);
    auto* rhs = dynamic_cast<Literal*>(bo->right.get());
    ASSERT_TRUE(rhs != nullptr);
    ASSERT_TRUE(rhs->kind == LiteralKind::STRING);
    ASSERT_EQ(rhs->sval, std::string("Alice"));
}

TEST(select_where_null_literal) {
    auto stmt = parseOne("slay * no-cap users only-if name = ghosted");
    auto* s = asSelect(stmt);
    auto* bo = dynamic_cast<BinaryOp*>(s->where.get());
    auto* rhs = dynamic_cast<Literal*>(bo->right.get());
    ASSERT_TRUE(rhs != nullptr);
    ASSERT_TRUE(rhs->kind == LiteralKind::NUL);
}

TEST(select_where_float_literal) {
    auto stmt = parseOne("slay * no-cap items only-if price > 3.14");
    auto* s = asSelect(stmt);
    auto* bo = dynamic_cast<BinaryOp*>(s->where.get());
    auto* rhs = dynamic_cast<Literal*>(bo->right.get());
    ASSERT_TRUE(rhs != nullptr);
    ASSERT_TRUE(rhs->kind == LiteralKind::FLOAT);
}

// -----------------------------------------------------------------------
// SELECT – GROUP BY, HAVING
// -----------------------------------------------------------------------
TEST(select_group_by) {
    auto stmt = parseOne("slay dept no-cap employees vibe-check dept");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->groupBy.size(), (size_t)1);
    auto* gr = dynamic_cast<ColumnRef*>(s->groupBy[0].get());
    ASSERT_TRUE(gr != nullptr);
    ASSERT_EQ(gr->column, std::string("dept"));
}

TEST(select_group_by_multiple_columns) {
    auto stmt = parseOne("slay dept, role no-cap employees vibe-check dept, role");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->groupBy.size(), (size_t)2);
}

TEST(select_having) {
    auto stmt = parseOne("slay dept no-cap employees vibe-check dept bussin-only headcount(*) > 5");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->having != nullptr);
    auto* h = dynamic_cast<BinaryOp*>(s->having.get());
    ASSERT_TRUE(h != nullptr);
    ASSERT_EQ(h->op, std::string(">"));
}

// -----------------------------------------------------------------------
// SELECT – ORDER BY
// -----------------------------------------------------------------------
TEST(select_order_by_asc) {
    auto stmt = parseOne("slay * no-cap users hits-different name up-only");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->orderBy.size(), (size_t)1);
    ASSERT_TRUE(s->orderBy[0].asc);
}

TEST(select_order_by_desc) {
    auto stmt = parseOne("slay * no-cap users hits-different id down-bad");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->orderBy.size(), (size_t)1);
    ASSERT_FALSE(s->orderBy[0].asc);
}

TEST(select_order_by_default_is_asc) {
    auto stmt = parseOne("slay * no-cap users hits-different name");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->orderBy[0].asc);
}

TEST(select_order_by_multiple_columns) {
    auto stmt = parseOne("slay * no-cap users hits-different dept up-only, name down-bad");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->orderBy.size(), (size_t)2);
    ASSERT_TRUE(s->orderBy[0].asc);
    ASSERT_FALSE(s->orderBy[1].asc);
}

// -----------------------------------------------------------------------
// SELECT – LIMIT and OFFSET
// -----------------------------------------------------------------------
TEST(select_limit) {
    auto stmt = parseOne("slay * no-cap users cap-at 10");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->limit != nullptr);
    auto* lit = dynamic_cast<Literal*>(s->limit.get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_EQ(lit->ival, (long long)10);
}

TEST(select_offset) {
    auto stmt = parseOne("slay * no-cap users cap-at 10 skip 5");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->offset != nullptr);
    auto* lit = dynamic_cast<Literal*>(s->offset.get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_EQ(lit->ival, (long long)5);
}

// -----------------------------------------------------------------------
// SELECT – JOINs
// -----------------------------------------------------------------------
TEST(select_inner_join_mid_link_up) {
    auto stmt = parseOne(
        "slay * no-cap users lowkey u mid-link-up orders lowkey o fr-fr u.id = o.user_id");
    auto* s = asSelect(stmt);
    ASSERT_EQ(s->joins.size(), (size_t)1);
    ASSERT_TRUE(s->joins[0].type == JoinType::INNER);
    ASSERT_EQ(s->joins[0].table, std::string("orders"));
    ASSERT_EQ(s->joins[0].alias, std::string("o"));
}

TEST(select_left_join) {
    auto stmt = parseOne(
        "slay * no-cap users lowkey u left-link-up orders lowkey o fr-fr u.id = o.user_id");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->joins[0].type == JoinType::LEFT);
}

TEST(select_regular_join_is_inner) {
    auto stmt = parseOne(
        "slay * no-cap users lowkey u link-up orders lowkey o fr-fr u.id = o.user_id");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->joins[0].type == JoinType::INNER);
}

TEST(select_join_has_on_condition) {
    auto stmt = parseOne(
        "slay * no-cap a link-up b fr-fr a.id = b.a_id");
    auto* s = asSelect(stmt);
    ASSERT_TRUE(s->joins[0].condition != nullptr);
    auto* cond = dynamic_cast<BinaryOp*>(s->joins[0].condition.get());
    ASSERT_EQ(cond->op, std::string("="));
}

TEST(select_qualified_column_ref) {
    auto stmt = parseOne("slay u.id no-cap users lowkey u");
    auto* s = asSelect(stmt);
    auto* cr = dynamic_cast<ColumnRef*>(s->columns[0].get());
    ASSERT_TRUE(cr != nullptr);
    ASSERT_EQ(cr->table, std::string("u"));
    ASSERT_EQ(cr->column, std::string("id"));
}

// -----------------------------------------------------------------------
// SELECT – aggregate functions
// -----------------------------------------------------------------------
TEST(select_headcount_star) {
    auto stmt = parseOne("slay headcount(*) no-cap users");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_TRUE(fc != nullptr);
    ASSERT_EQ(fc->name, std::string("headcount"));
    ASSERT_FALSE(fc->distinct);
    ASSERT_EQ(fc->args.size(), (size_t)1);
    ASSERT_TRUE(dynamic_cast<Wildcard*>(fc->args[0].get()) != nullptr);
}

TEST(select_headcount_distinct) {
    auto stmt = parseOne("slay headcount(unique-fr id) no-cap users");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_TRUE(fc != nullptr);
    ASSERT_TRUE(fc->distinct);
    ASSERT_EQ(fc->args.size(), (size_t)1);
}

TEST(select_sum_function) {
    auto stmt = parseOne("slay stack(amount) no-cap orders");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_EQ(fc->name, std::string("stack"));
}

TEST(select_avg_function) {
    auto stmt = parseOne("slay mid(score) no-cap results");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_EQ(fc->name, std::string("mid"));
}

TEST(select_max_function) {
    auto stmt = parseOne("slay goat(salary) no-cap employees");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_EQ(fc->name, std::string("goat"));
}

TEST(select_min_function_L) {
    auto stmt = parseOne("slay L(salary) no-cap employees");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_TRUE(fc != nullptr);
    ASSERT_EQ(fc->name, std::string("L"));
}

TEST(select_median_function) {
    auto stmt = parseOne("slay mid-fr(salary) no-cap employees");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_EQ(fc->name, std::string("mid-fr"));
}

TEST(select_percentile_function) {
    auto stmt = parseOne("slay percent-check(salary, 90) no-cap employees");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_EQ(fc->name, std::string("percent-check"));
    ASSERT_EQ(fc->args.size(), (size_t)2);
}

TEST(select_argmax_biggest_W) {
    auto stmt = parseOne("slay biggest-W(salary) no-cap employees");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_EQ(fc->name, std::string("biggest-W"));
}

TEST(select_argmin_biggest_L) {
    auto stmt = parseOne("slay biggest-L(salary) no-cap employees");
    auto* s = asSelect(stmt);
    auto* fc = dynamic_cast<FunctionCall*>(s->columns[0].get());
    ASSERT_EQ(fc->name, std::string("biggest-L"));
}

// -----------------------------------------------------------------------
// SELECT – window function (era)
// -----------------------------------------------------------------------
TEST(select_era_window_function_basic) {
    auto stmt = parseOne("slay era hits-different salary down-bad no-cap employees");
    auto* s = asSelect(stmt);
    auto* wf = dynamic_cast<WindowFunc*>(s->columns[0].get());
    ASSERT_TRUE(wf != nullptr);
    ASSERT_EQ(wf->funcName, std::string("RANK"));
    ASSERT_EQ(wf->order_by.size(), (size_t)1);
    ASSERT_FALSE(wf->order_by[0].asc);
}

TEST(select_era_with_partition_by) {
    auto stmt = parseOne("slay era split-by dept hits-different salary no-cap employees");
    auto* s = asSelect(stmt);
    auto* wf = dynamic_cast<WindowFunc*>(s->columns[0].get());
    ASSERT_TRUE(wf != nullptr);
    ASSERT_EQ(wf->partition_by.size(), (size_t)1);
    ASSERT_EQ(wf->order_by.size(), (size_t)1);
}

// -----------------------------------------------------------------------
// SELECT – expression parsing (operators)
// -----------------------------------------------------------------------
TEST(select_arithmetic_expression) {
    auto stmt = parseOne("slay price + tax no-cap items");
    auto* s = asSelect(stmt);
    auto* bo = dynamic_cast<BinaryOp*>(s->columns[0].get());
    ASSERT_TRUE(bo != nullptr);
    ASSERT_EQ(bo->op, std::string("+"));
}

TEST(select_arithmetic_multiplication) {
    auto stmt = parseOne("slay qty * price no-cap items");
    auto* s = asSelect(stmt);
    auto* bo = dynamic_cast<BinaryOp*>(s->columns[0].get());
    ASSERT_EQ(bo->op, std::string("*"));
}

TEST(select_string_concat_expr) {
    auto stmt = parseOne("slay first_name || ' ' || last_name no-cap users");
    auto* s = asSelect(stmt);
    // The outer concat should be BinaryOp with ||
    auto* bo = dynamic_cast<BinaryOp*>(s->columns[0].get());
    ASSERT_TRUE(bo != nullptr);
    ASSERT_EQ(bo->op, std::string("||"));
}

TEST(select_unary_minus) {
    auto stmt = parseOne("slay -1 no-cap t");
    auto* s = asSelect(stmt);
    auto* uo = dynamic_cast<UnaryOp*>(s->columns[0].get());
    ASSERT_TRUE(uo != nullptr);
    ASSERT_EQ(uo->op, std::string("-"));
}

TEST(select_parenthesized_expr) {
    auto stmt = parseOne("slay (a + b) * c no-cap t");
    auto* s = asSelect(stmt);
    auto* outer = dynamic_cast<BinaryOp*>(s->columns[0].get());
    ASSERT_TRUE(outer != nullptr);
    ASSERT_EQ(outer->op, std::string("*"));
}

// -----------------------------------------------------------------------
// INSERT
// -----------------------------------------------------------------------
TEST(insert_with_column_list) {
    auto stmt = parseOne("yeet-into users (id, name) drip (1, 'Alice')");
    auto* s = asInsert(stmt);
    ASSERT_EQ(s->table, std::string("users"));
    ASSERT_EQ(s->columns.size(), (size_t)2);
    ASSERT_EQ(s->columns[0], std::string("id"));
    ASSERT_EQ(s->columns[1], std::string("name"));
    ASSERT_EQ(s->valueRows.size(), (size_t)1);
    ASSERT_EQ(s->valueRows[0].size(), (size_t)2);
}

TEST(insert_without_column_list) {
    auto stmt = parseOne("yeet-into users drip (1, 'Alice')");
    auto* s = asInsert(stmt);
    ASSERT_EQ(s->table, std::string("users"));
    ASSERT_TRUE(s->columns.empty());
    ASSERT_EQ(s->valueRows.size(), (size_t)1);
}

TEST(insert_multiple_rows) {
    auto stmt = parseOne("yeet-into users (id, name) drip (1, 'Alice'), (2, 'Bob')");
    auto* s = asInsert(stmt);
    ASSERT_EQ(s->valueRows.size(), (size_t)2);
}

TEST(insert_null_value) {
    auto stmt = parseOne("yeet-into users (id, name) drip (1, ghosted)");
    auto* s = asInsert(stmt);
    auto* val = dynamic_cast<Literal*>(s->valueRows[0][1].get());
    ASSERT_TRUE(val != nullptr);
    ASSERT_TRUE(val->kind == LiteralKind::NUL);
}

TEST(insert_float_value) {
    auto stmt = parseOne("yeet-into prices (id, amount) drip (1, 9.99)");
    auto* s = asInsert(stmt);
    auto* val = dynamic_cast<Literal*>(s->valueRows[0][1].get());
    ASSERT_TRUE(val != nullptr);
    ASSERT_TRUE(val->kind == LiteralKind::FLOAT);
}

// -----------------------------------------------------------------------
// UPDATE
// -----------------------------------------------------------------------
TEST(update_single_set_with_where) {
    auto stmt = parseOne("glow-up users be-like name = 'Alice' only-if id = 1");
    auto* s = asUpdate(stmt);
    ASSERT_EQ(s->table, std::string("users"));
    ASSERT_EQ(s->sets.size(), (size_t)1);
    ASSERT_EQ(s->sets[0].column, std::string("name"));
    ASSERT_TRUE(s->where != nullptr);
}

TEST(update_without_where) {
    auto stmt = parseOne("glow-up users be-like active = 0");
    auto* s = asUpdate(stmt);
    ASSERT_EQ(s->sets.size(), (size_t)1);
    ASSERT_TRUE(s->where == nullptr);
}

TEST(update_multiple_sets) {
    auto stmt = parseOne("glow-up users be-like name = 'Bob', active = 1");
    auto* s = asUpdate(stmt);
    ASSERT_EQ(s->sets.size(), (size_t)2);
    ASSERT_EQ(s->sets[0].column, std::string("name"));
    ASSERT_EQ(s->sets[1].column, std::string("active"));
}

TEST(update_set_value_is_expression) {
    auto stmt = parseOne("glow-up orders be-like total = price * qty");
    auto* s = asUpdate(stmt);
    auto* val = dynamic_cast<BinaryOp*>(s->sets[0].value.get());
    ASSERT_TRUE(val != nullptr);
    ASSERT_EQ(val->op, std::string("*"));
}

// -----------------------------------------------------------------------
// DELETE
// -----------------------------------------------------------------------
TEST(delete_with_where) {
    auto stmt = parseOne("ratio users only-if id = 1");
    auto* s = asDelete(stmt);
    ASSERT_EQ(s->table, std::string("users"));
    ASSERT_TRUE(s->where != nullptr);
}

TEST(delete_without_where) {
    auto stmt = parseOne("ratio temp_table");
    auto* s = asDelete(stmt);
    ASSERT_EQ(s->table, std::string("temp_table"));
    ASSERT_TRUE(s->where == nullptr);
}

TEST(delete_complex_where) {
    auto stmt = parseOne("ratio users only-if id > 10 plus active = 0");
    auto* s = asDelete(stmt);
    auto* w = dynamic_cast<BinaryOp*>(s->where.get());
    ASSERT_TRUE(w != nullptr);
    ASSERT_EQ(w->op, std::string("AND"));
}

// -----------------------------------------------------------------------
// CREATE TABLE
// -----------------------------------------------------------------------
TEST(create_table_basic) {
    auto stmt = parseOne("manifest users (id INTEGER, name TEXT)");
    auto* s = asCreate(stmt);
    ASSERT_EQ(s->table, std::string("users"));
    ASSERT_EQ(s->columns.size(), (size_t)2);
    ASSERT_EQ(s->columns[0].name, std::string("id"));
    ASSERT_EQ(s->columns[0].type, std::string("INTEGER"));
    ASSERT_EQ(s->columns[1].name, std::string("name"));
    ASSERT_EQ(s->columns[1].type, std::string("TEXT"));
}

TEST(create_table_primary_key) {
    auto stmt = parseOne("manifest users (id INTEGER main-character, name TEXT)");
    auto* s = asCreate(stmt);
    ASSERT_TRUE(s->columns[0].primary_key);
    ASSERT_FALSE(s->columns[1].primary_key);
}

TEST(create_table_not_null) {
    auto stmt = parseOne("manifest users (id INTEGER, name TEXT no-cap-not ghosted)");
    auto* s = asCreate(stmt);
    ASSERT_FALSE(s->columns[0].not_null);
    ASSERT_TRUE(s->columns[1].not_null);
}

TEST(create_table_foreign_key) {
    auto stmt = parseOne(
        "manifest orders (id INTEGER main-character, user_id INTEGER side-character references users(id))");
    auto* s = asCreate(stmt);
    ASSERT_EQ(s->columns[1].fk_table, std::string("users"));
    ASSERT_EQ(s->columns[1].fk_col, std::string("id"));
}

TEST(create_table_multiple_constraints) {
    auto stmt = parseOne("manifest t (id INTEGER main-character no-cap-not ghosted)");
    auto* s = asCreate(stmt);
    ASSERT_TRUE(s->columns[0].primary_key);
    ASSERT_TRUE(s->columns[0].not_null);
}

TEST(create_table_real_type) {
    auto stmt = parseOne("manifest prices (id INTEGER, amount REAL)");
    auto* s = asCreate(stmt);
    ASSERT_EQ(s->columns[1].type, std::string("REAL"));
}

TEST(create_table_blob_type) {
    auto stmt = parseOne("manifest files (id INTEGER, data BLOB)");
    auto* s = asCreate(stmt);
    ASSERT_EQ(s->columns[1].type, std::string("BLOB"));
}

// -----------------------------------------------------------------------
// DROP TABLE
// -----------------------------------------------------------------------
TEST(drop_table_basic) {
    auto stmt = parseOne("rizz-down users");
    auto* s = asDrop(stmt);
    ASSERT_EQ(s->table, std::string("users"));
}

TEST(drop_table_name_preserved) {
    auto stmt = parseOne("rizz-down order_history");
    auto* s = asDrop(stmt);
    ASSERT_EQ(s->table, std::string("order_history"));
}

// -----------------------------------------------------------------------
// Error cases – ParseError thrown
// -----------------------------------------------------------------------
TEST(select_missing_no_cap_throws) {
    ASSERT_THROW(parse("slay *"), ParseError);
}

TEST(select_missing_table_name_throws) {
    ASSERT_THROW(parse("slay * no-cap"), ParseError);
}

TEST(insert_missing_drip_throws) {
    ASSERT_THROW(parse("yeet-into users (id)"), ParseError);
}

TEST(update_missing_be_like_throws) {
    ASSERT_THROW(parse("glow-up users id = 1"), ParseError);
}

TEST(create_missing_paren_throws) {
    ASSERT_THROW(parse("manifest users id INTEGER"), ParseError);
}

TEST(era_missing_hits_different_throws) {
    ASSERT_THROW(parse("slay era no-cap t"), ParseError);
}

TEST(parse_error_has_line_col) {
    try {
        parse("slay *");
        ASSERT_TRUE(false);
    } catch (const ParseError& e) {
        ASSERT_TRUE(e.line >= 1);
        ASSERT_TRUE(e.col >= 1);
    }
}

int main() {
    return run_all_tests("Parser");
}
