// Tests for Optimizer::optimize()
// Covers all 4 optimization passes:
//   1. Constant folding (arithmetic, comparison, string concat, boolean, unary)
//   2. Predicate pushdown (annotation / reporting)
//   3. Projection pruning (annotation / reporting)
//   4. Dead code elimination (always-true / always-false WHERE / HAVING)
//
// Design note: constant-folding value tests place the expression in the
// SELECT column list (not WHERE) to avoid dead-code elimination removing
// the folded truthy literal before the test can inspect it.
#include "test_framework.h"
#include "lexer.h"
#include "parser.h"
#include "optimizer.h"
#include "codegen.h"
#include "ast.h"
#include <memory>
#include <string>
#include <algorithm>

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Build a SelectStmt with a WHERE clause.
static std::unique_ptr<SelectStmt> makeSelectWithWhere(
    std::unique_ptr<ASTNode> whereExpr)
{
    auto sel = std::make_unique<SelectStmt>();
    sel->fromTable = "t";
    sel->columns.push_back(std::make_unique<Wildcard>());
    sel->where = std::move(whereExpr);
    return sel;
}

// Build a SelectStmt with the expression as the first column.
// Dead code elimination does NOT affect column expressions, so folded
// literal values survive in the column list even when truthy.
static std::unique_ptr<SelectStmt> makeSelectWithCol(
    std::unique_ptr<ASTNode> colExpr)
{
    auto sel = std::make_unique<SelectStmt>();
    sel->fromTable = "t";
    sel->columns.push_back(std::move(colExpr));
    return sel;
}

static std::unique_ptr<BinaryOp> makeBinOp(
    std::unique_ptr<ASTNode> l, const std::string& op, std::unique_ptr<ASTNode> r)
{
    auto b = std::make_unique<BinaryOp>();
    b->op = op;
    b->left = std::move(l);
    b->right = std::move(r);
    return b;
}

static bool reportContains(const OptimizationReport& r, const std::string& needle) {
    for (auto& n : r.notes)
        if (n.find(needle) != std::string::npos) return true;
    return false;
}

// Run the optimizer on a SkibidiQL source string.
static std::pair<std::unique_ptr<ASTNode>, OptimizationReport>
optimizeSource(const std::string& src) {
    auto toks = Lexer(src).tokenize();
    Parser p(std::move(toks));
    auto stmts = p.parseAll();
    ASSERT_EQ(stmts.size(), (size_t)1);
    Optimizer opt;
    OptimizationReport report;
    auto result = opt.optimize(std::move(stmts[0]), report);
    return {std::move(result), std::move(report)};
}

// -----------------------------------------------------------------------
// Optimizer constructor
// -----------------------------------------------------------------------
TEST(optimizer_constructs_without_throw) {
    ASSERT_NO_THROW({ Optimizer opt(false); });
    ASSERT_NO_THROW({ Optimizer opt(true); });
}

// -----------------------------------------------------------------------
// OptimizationReport
// -----------------------------------------------------------------------
TEST(optimization_report_add_stores_note) {
    OptimizationReport r;
    r.add("test note");
    ASSERT_EQ(r.notes.size(), (size_t)1);
    ASSERT_EQ(r.notes[0], std::string("test note"));
}

TEST(optimization_report_multiple_notes) {
    OptimizationReport r;
    r.add("note1");
    r.add("note2");
    ASSERT_EQ(r.notes.size(), (size_t)2);
}

// -----------------------------------------------------------------------
// Pass 1: Constant folding – integer arithmetic
// (expression placed in column list so dead-code elim cannot remove it)
// -----------------------------------------------------------------------
TEST(constant_folding_int_add) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(2), "+", Literal::makeInt(3)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::INT);
    ASSERT_EQ(lit->ival, (long long)5);
    ASSERT_TRUE(reportContains(r, "Constant folding"));
}

TEST(constant_folding_int_subtract) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(10), "-", Literal::makeInt(3)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_EQ(lit->ival, (long long)7);
}

TEST(constant_folding_int_multiply) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(4), "*", Literal::makeInt(5)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_EQ(lit->ival, (long long)20);
}

TEST(constant_folding_int_divide) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(10), "/", Literal::makeInt(2)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_EQ(lit->ival, (long long)5);
    ASSERT_TRUE(lit->kind == LiteralKind::INT);
}

TEST(constant_folding_divide_by_zero_not_folded) {
    // Division by zero must not be folded; expression should stay as BinaryOp
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(10), "/", Literal::makeInt(0)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    ASSERT_TRUE(dynamic_cast<BinaryOp*>(s->columns[0].get()) != nullptr);
}

// -----------------------------------------------------------------------
// Pass 1: Constant folding – float arithmetic
// -----------------------------------------------------------------------
TEST(constant_folding_float_add) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeFloat(1.5), "+", Literal::makeFloat(2.5)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::FLOAT);
    ASSERT_TRUE(lit->fval > 3.99 && lit->fval < 4.01);
}

TEST(constant_folding_float_multiply) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeFloat(2.0), "*", Literal::makeFloat(3.5)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::FLOAT);
    ASSERT_TRUE(lit->fval > 6.99 && lit->fval < 7.01);
}

TEST(constant_folding_mixed_int_float_produces_float) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(2), "+", Literal::makeFloat(1.5)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::FLOAT);
    ASSERT_TRUE(lit->fval > 3.49 && lit->fval < 3.51);
}

// -----------------------------------------------------------------------
// Pass 1: Constant folding – comparisons → bool literal
// -----------------------------------------------------------------------
TEST(constant_folding_eq_true_result) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(1), "=", Literal::makeInt(1)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::BOOL);
    ASSERT_TRUE(lit->bval);
    ASSERT_TRUE(reportContains(r, "comparison"));
}

TEST(constant_folding_eq_false_result) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(1), "=", Literal::makeInt(2)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::BOOL);
    ASSERT_FALSE(lit->bval);
}

TEST(constant_folding_neq) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(1), "!=", Literal::makeInt(2)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->bval);
}

TEST(constant_folding_lt_true) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(3), "<", Literal::makeInt(5)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && lit->bval);
}

TEST(constant_folding_gt_false) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(3), ">", Literal::makeInt(5)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && !lit->bval);
}

TEST(constant_folding_lte_equal) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(5), "<=", Literal::makeInt(5)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && lit->bval);
}

TEST(constant_folding_gte_false) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeInt(5), ">=", Literal::makeInt(6)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && !lit->bval);
}

// -----------------------------------------------------------------------
// Pass 1: Constant folding – string concatenation
// -----------------------------------------------------------------------
TEST(constant_folding_string_concat) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeString("hello"), "||", Literal::makeString(" world")));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::STRING);
    ASSERT_EQ(lit->sval, std::string("hello world"));
    ASSERT_TRUE(reportContains(r, "string concatenation"));
}

TEST(constant_folding_string_concat_empty) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeString(""), "||", Literal::makeString("abc")));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_EQ(lit->sval, std::string("abc"));
}

// -----------------------------------------------------------------------
// Pass 1: Constant folding – boolean AND / OR
// -----------------------------------------------------------------------
TEST(constant_folding_bool_and_true_true) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeBool(true), "AND", Literal::makeBool(true)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && lit->bval);
    ASSERT_TRUE(reportContains(r, "AND"));
}

TEST(constant_folding_bool_and_false) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeBool(true), "AND", Literal::makeBool(false)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && !lit->bval);
    ASSERT_TRUE(reportContains(r, "AND"));
}

TEST(constant_folding_bool_or_false_true) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeBool(false), "OR", Literal::makeBool(true)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && lit->bval);
    ASSERT_TRUE(reportContains(r, "OR"));
}

TEST(constant_folding_bool_or_both_false) {
    auto sel = makeSelectWithCol(
        makeBinOp(Literal::makeBool(false), "OR", Literal::makeBool(false)));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && !lit->bval);
}

// -----------------------------------------------------------------------
// Pass 1: Constant folding – unary minus
// -----------------------------------------------------------------------
TEST(constant_folding_unary_minus_int) {
    auto unop = std::make_unique<UnaryOp>();
    unop->op = "-";
    unop->operand = Literal::makeInt(5);
    auto sel = makeSelectWithCol(std::move(unop));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::INT);
    ASSERT_EQ(lit->ival, (long long)-5);
    ASSERT_TRUE(reportContains(r, "unary minus"));
}

TEST(constant_folding_unary_minus_float) {
    auto unop = std::make_unique<UnaryOp>();
    unop->op = "-";
    unop->operand = Literal::makeFloat(3.14);
    auto sel = makeSelectWithCol(std::move(unop));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_TRUE(lit->kind == LiteralKind::FLOAT);
    ASSERT_TRUE(lit->fval < -3.0);
    ASSERT_TRUE(reportContains(r, "unary minus"));
}

TEST(constant_folding_unary_minus_zero_stays_int) {
    auto unop = std::make_unique<UnaryOp>();
    unop->op = "-";
    unop->operand = Literal::makeInt(0);
    auto sel = makeSelectWithCol(std::move(unop));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_EQ(lit->ival, (long long)0);
}

// -----------------------------------------------------------------------
// Pass 1: Constant folding – NOT on bool literal
// -----------------------------------------------------------------------
TEST(constant_folding_not_true_becomes_false) {
    auto unop = std::make_unique<UnaryOp>();
    unop->op = "NOT";
    unop->operand = Literal::makeBool(true);
    auto sel = makeSelectWithCol(std::move(unop));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && !lit->bval);
    ASSERT_TRUE(reportContains(r, "NOT"));
}

TEST(constant_folding_not_false_becomes_true) {
    auto unop = std::make_unique<UnaryOp>();
    unop->op = "NOT";
    unop->operand = Literal::makeBool(false);
    auto sel = makeSelectWithCol(std::move(unop));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr && lit->bval);
    ASSERT_TRUE(reportContains(r, "NOT"));
}

// -----------------------------------------------------------------------
// Pass 1: Nested constant folding
// -----------------------------------------------------------------------
TEST(constant_folding_nested_add_then_multiply) {
    // (2 + 3) * 4 = 20
    auto inner = makeBinOp(Literal::makeInt(2), "+", Literal::makeInt(3));
    auto outer = makeBinOp(std::move(inner), "*", Literal::makeInt(4));
    auto sel = makeSelectWithCol(std::move(outer));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    auto* lit = dynamic_cast<Literal*>(s->columns[0].get());
    ASSERT_TRUE(lit != nullptr);
    ASSERT_EQ(lit->ival, (long long)20);
}

// -----------------------------------------------------------------------
// Pass 4: Dead code elimination – always-true WHERE removed
// -----------------------------------------------------------------------
TEST(dead_code_elimination_always_true_bool_where_removed) {
    auto sel = makeSelectWithWhere(Literal::makeBool(true));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    ASSERT_TRUE(s->where == nullptr);
    ASSERT_TRUE(reportContains(r, "TRUE"));
}

TEST(dead_code_elimination_always_true_nonzero_int_where_removed) {
    auto sel = makeSelectWithWhere(Literal::makeInt(1));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    ASSERT_TRUE(s->where == nullptr);
}

TEST(dead_code_elimination_nonzero_string_where_removed) {
    auto sel = makeSelectWithWhere(Literal::makeString("nonempty"));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    // Non-empty string is truthy → WHERE removed
    ASSERT_TRUE(s->where == nullptr);
}

TEST(dead_code_elimination_always_false_bool_where_kept_and_reported) {
    auto sel = makeSelectWithWhere(Literal::makeBool(false));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    // WHERE is always false: should be reported but the WHERE clause is retained
    ASSERT_TRUE(reportContains(r, "FALSE"));
    // The WHERE node should remain (query must return 0 rows, not be misoptimized)
    ASSERT_TRUE(s->where != nullptr);
}

TEST(dead_code_elimination_zero_int_where_reported) {
    auto sel = makeSelectWithWhere(Literal::makeInt(0));
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    ASSERT_TRUE(reportContains(r, "FALSE"));
    ASSERT_TRUE(s->where != nullptr);
}

TEST(dead_code_elimination_null_where_reported_as_false) {
    auto sel = makeSelectWithWhere(Literal::makeNull());
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    // NULL is falsy: should be reported
    ASSERT_TRUE(reportContains(r, "FALSE"));
}

TEST(dead_code_elimination_always_true_having_removed) {
    auto sel = std::make_unique<SelectStmt>();
    sel->fromTable = "t";
    sel->columns.push_back(std::make_unique<Wildcard>());
    sel->having = Literal::makeBool(true);
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    ASSERT_TRUE(s->having == nullptr);
    ASSERT_TRUE(reportContains(r, "TRUE"));
}

TEST(dead_code_elimination_always_false_having_reported) {
    auto sel = std::make_unique<SelectStmt>();
    sel->fromTable = "t";
    sel->columns.push_back(std::make_unique<Wildcard>());
    sel->having = Literal::makeBool(false);
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(sel), r);
    ASSERT_TRUE(reportContains(r, "FALSE"));
}

// -----------------------------------------------------------------------
// Pass 2: Predicate pushdown – reporting
// -----------------------------------------------------------------------
TEST(predicate_pushdown_where_without_join_reported) {
    auto [result, r] = optimizeSource("slay * no-cap users only-if id = 1");
    ASSERT_TRUE(reportContains(r, "Predicate pushdown"));
    ASSERT_TRUE(reportContains(r, "users"));
}

TEST(predicate_pushdown_no_where_no_report) {
    auto [result, r] = optimizeSource("slay * no-cap users");
    ASSERT_FALSE(reportContains(r, "Predicate pushdown"));
}

TEST(predicate_pushdown_where_with_join_reports_filter_after_join) {
    auto [result, r] = optimizeSource(
        "slay * no-cap users lowkey u link-up orders lowkey o "
        "fr-fr u.id = o.user_id only-if u.active = 1");
    ASSERT_TRUE(reportContains(r, "Predicate pushdown"));
    ASSERT_TRUE(reportContains(r, "JOIN"));
}

// -----------------------------------------------------------------------
// Pass 3: Projection pruning – reporting
// -----------------------------------------------------------------------
TEST(projection_pruning_wildcard_reported) {
    auto [result, r] = optimizeSource("slay * no-cap users");
    ASSERT_TRUE(reportContains(r, "Projection pruning"));
    ASSERT_TRUE(reportContains(r, "SELECT *") || reportContains(r, "all columns"));
}

TEST(projection_pruning_two_columns_reported) {
    auto [result, r] = optimizeSource("slay id, name no-cap users");
    ASSERT_TRUE(reportContains(r, "Projection pruning"));
    ASSERT_TRUE(reportContains(r, "2"));
}

TEST(projection_pruning_single_column_reported) {
    auto [result, r] = optimizeSource("slay id no-cap users");
    ASSERT_TRUE(reportContains(r, "Projection pruning"));
    ASSERT_TRUE(reportContains(r, "1"));
}

// -----------------------------------------------------------------------
// Non-constant expressions are not folded
// -----------------------------------------------------------------------
TEST(non_constant_where_not_folded) {
    auto [result, r] = optimizeSource("slay * no-cap users only-if id = 1");
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    // id = 1 involves a column (not all-constant) → WHERE is kept
    ASSERT_TRUE(s->where != nullptr);
}

TEST(non_constant_column_expression_not_folded) {
    auto [result, r] = optimizeSource("slay price * qty no-cap orders");
    auto* s = dynamic_cast<SelectStmt*>(result.get());
    // price * qty are not literals → stays as BinaryOp
    ASSERT_TRUE(dynamic_cast<BinaryOp*>(s->columns[0].get()) != nullptr);
}

// -----------------------------------------------------------------------
// optimize() edge cases
// -----------------------------------------------------------------------
TEST(optimize_returns_non_null_for_select) {
    auto toks = Lexer("slay * no-cap users").tokenize();
    Parser p(std::move(toks));
    auto stmts = p.parseAll();
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(std::move(stmts[0]), r);
    ASSERT_TRUE(result != nullptr);
}

TEST(optimize_null_input_returns_null) {
    Optimizer opt;
    OptimizationReport r;
    auto result = opt.optimize(nullptr, r);
    ASSERT_TRUE(result == nullptr);
}

TEST(optimize_all_four_passes_run_on_select) {
    // A real query should trigger at least one note from each pass
    auto [result, r] = optimizeSource("slay id no-cap users only-if id = 1");
    // Projection pruning note must exist
    ASSERT_TRUE(reportContains(r, "Projection pruning"));
    // Predicate pushdown note must exist
    ASSERT_TRUE(reportContains(r, "Predicate pushdown"));
}

int main() {
    return run_all_tests("Optimizer");
}
