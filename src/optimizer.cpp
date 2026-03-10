#include "optimizer.h"
#include <cmath>
#include <stdexcept>

Optimizer::Optimizer(bool verbose) : verbose_(verbose) {}

std::unique_ptr<ASTNode> Optimizer::optimize(std::unique_ptr<ASTNode> ast, OptimizationReport& report) {
    // Pass 1: constant folding
    ast = foldConstants(std::move(ast), report);

    // Pass 2: predicate pushdown (analysis only)
    pushdownPredicates(ast.get(), report);

    // Pass 3: projection pruning (analysis only)
    pruneProjections(ast.get(), report);

    // Pass 4: dead code elimination
    eliminateDeadCode(ast.get(), report);

    return ast;
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
bool Optimizer::isConstant(const ASTNode* node) const {
    if (!node) return false;
    if (dynamic_cast<const Literal*>(node)) return true;
    if (auto* b = dynamic_cast<const BinaryOp*>(node)) {
        return isConstant(b->left.get()) && isConstant(b->right.get());
    }
    if (auto* u = dynamic_cast<const UnaryOp*>(node)) {
        return isConstant(u->operand.get());
    }
    return false;
}

bool Optimizer::isTruthy(const Literal* lit) const {
    if (!lit) return false;
    switch (lit->kind) {
        case LiteralKind::INT:    return lit->ival != 0;
        case LiteralKind::FLOAT:  return lit->fval != 0.0;
        case LiteralKind::STRING: return !lit->sval.empty();
        case LiteralKind::BOOL:   return lit->bval;
        case LiteralKind::NUL:    return false;
    }
    return false;
}

bool Optimizer::isFalsy(const Literal* lit) const {
    return !isTruthy(lit);
}

// -----------------------------------------------------------------------
// Pass 1: Constant folding
// -----------------------------------------------------------------------
std::unique_ptr<ASTNode> Optimizer::tryFoldExpr(ASTNode* node, OptimizationReport& r) {
    if (!node) return nullptr;

    auto* binop = dynamic_cast<BinaryOp*>(node);
    if (!binop) return nullptr;

    auto* llit = dynamic_cast<Literal*>(binop->left.get());
    auto* rlit = dynamic_cast<Literal*>(binop->right.get());
    if (!llit || !rlit) return nullptr;

    const std::string& op = binop->op;
    int l = node->line, c = node->col;

    // Arithmetic on numbers
    if ((llit->kind == LiteralKind::INT || llit->kind == LiteralKind::FLOAT) &&
        (rlit->kind == LiteralKind::INT || rlit->kind == LiteralKind::FLOAT)) {

        bool bothInt = (llit->kind == LiteralKind::INT && rlit->kind == LiteralKind::INT);
        double lv = (llit->kind == LiteralKind::INT) ? (double)llit->ival : llit->fval;
        double rv = (rlit->kind == LiteralKind::INT) ? (double)rlit->ival : rlit->fval;

        if (op == "+") {
            r.add("Constant folding: " + std::to_string(lv) + " + " + std::to_string(rv));
            if (bothInt) return Literal::makeInt(llit->ival + rlit->ival, l, c);
            return Literal::makeFloat(lv + rv, l, c);
        }
        if (op == "-") {
            r.add("Constant folding: " + std::to_string(lv) + " - " + std::to_string(rv));
            if (bothInt) return Literal::makeInt(llit->ival - rlit->ival, l, c);
            return Literal::makeFloat(lv - rv, l, c);
        }
        if (op == "*") {
            r.add("Constant folding: " + std::to_string(lv) + " * " + std::to_string(rv));
            if (bothInt) return Literal::makeInt(llit->ival * rlit->ival, l, c);
            return Literal::makeFloat(lv * rv, l, c);
        }
        if (op == "/" && rv != 0.0) {
            r.add("Constant folding: " + std::to_string(lv) + " / " + std::to_string(rv));
            if (bothInt && (rlit->ival != 0)) return Literal::makeInt(llit->ival / rlit->ival, l, c);
            return Literal::makeFloat(lv / rv, l, c);
        }
        // Comparison operators -> bool
        if (op == "=")  { r.add("Constant folding: comparison"); return Literal::makeBool(lv == rv, l, c); }
        if (op == "!=") { r.add("Constant folding: comparison"); return Literal::makeBool(lv != rv, l, c); }
        if (op == "<")  { r.add("Constant folding: comparison"); return Literal::makeBool(lv <  rv, l, c); }
        if (op == ">")  { r.add("Constant folding: comparison"); return Literal::makeBool(lv >  rv, l, c); }
        if (op == "<=") { r.add("Constant folding: comparison"); return Literal::makeBool(lv <= rv, l, c); }
        if (op == ">=") { r.add("Constant folding: comparison"); return Literal::makeBool(lv >= rv, l, c); }
    }

    // String concatenation
    if (op == "||" && llit->kind == LiteralKind::STRING && rlit->kind == LiteralKind::STRING) {
        r.add("Constant folding: string concatenation");
        return Literal::makeString(llit->sval + rlit->sval, l, c);
    }

    // Boolean logic
    if (op == "AND") {
        if (llit->kind == LiteralKind::BOOL && rlit->kind == LiteralKind::BOOL) {
            r.add("Constant folding: AND");
            return Literal::makeBool(llit->bval && rlit->bval, l, c);
        }
    }
    if (op == "OR") {
        if (llit->kind == LiteralKind::BOOL && rlit->kind == LiteralKind::BOOL) {
            r.add("Constant folding: OR");
            return Literal::makeBool(llit->bval || rlit->bval, l, c);
        }
    }

    return nullptr; // can't fold
}

std::unique_ptr<ASTNode> Optimizer::foldConstants(std::unique_ptr<ASTNode> node, OptimizationReport& r) {
    if (!node) return nullptr;

    if (auto* sel = dynamic_cast<SelectStmt*>(node.get())) {
        // Fold expressions in columns
        for (auto& col : sel->columns) {
            col = foldConstants(std::move(col), r);
        }
        // Fold WHERE
        if (sel->where) {
            sel->where = foldConstants(std::move(sel->where), r);
        }
        // Fold HAVING
        if (sel->having) {
            sel->having = foldConstants(std::move(sel->having), r);
        }
        // Fold LIMIT / OFFSET
        if (sel->limit) sel->limit = foldConstants(std::move(sel->limit), r);
        if (sel->offset) sel->offset = foldConstants(std::move(sel->offset), r);
        return node;
    }

    if (auto* binop = dynamic_cast<BinaryOp*>(node.get())) {
        binop->left = foldConstants(std::move(binop->left), r);
        binop->right = foldConstants(std::move(binop->right), r);
        // Try to fold this node itself
        auto folded = tryFoldExpr(node.get(), r);
        if (folded) return folded;
        return node;
    }

    if (auto* unop = dynamic_cast<UnaryOp*>(node.get())) {
        unop->operand = foldConstants(std::move(unop->operand), r);
        // Fold unary minus on literal
        if (unop->op == "-") {
            if (auto* lit = dynamic_cast<Literal*>(unop->operand.get())) {
                if (lit->kind == LiteralKind::INT) {
                    r.add("Constant folding: unary minus");
                    return Literal::makeInt(-lit->ival, node->line, node->col);
                }
                if (lit->kind == LiteralKind::FLOAT) {
                    r.add("Constant folding: unary minus");
                    return Literal::makeFloat(-lit->fval, node->line, node->col);
                }
            }
        }
        if (unop->op == "NOT") {
            if (auto* lit = dynamic_cast<Literal*>(unop->operand.get())) {
                if (lit->kind == LiteralKind::BOOL) {
                    r.add("Constant folding: NOT");
                    return Literal::makeBool(!lit->bval, node->line, node->col);
                }
            }
        }
        return node;
    }

    if (auto* fc = dynamic_cast<FunctionCall*>(node.get())) {
        for (auto& arg : fc->args) {
            arg = foldConstants(std::move(arg), r);
        }
        return node;
    }

    return node;
}

// -----------------------------------------------------------------------
// Pass 2: Predicate pushdown (analysis/annotation)
// -----------------------------------------------------------------------
void Optimizer::pushdownPredicates(ASTNode* node, OptimizationReport& r) {
    if (!node) return;

    if (auto* sel = dynamic_cast<SelectStmt*>(node)) {
        if (sel->where && sel->joins.empty()) {
            r.add("Predicate pushdown: WHERE condition applied at scan level for table '" + sel->fromTable + "'");
        }
        if (sel->where && !sel->joins.empty()) {
            r.add("Predicate pushdown: WHERE condition with JOINs - filter applied after join");
        }
        // Recurse into subqueries in columns (SelectStmt nodes)
        for (auto& col : sel->columns) {
            if (auto* sub = dynamic_cast<SelectStmt*>(col.get())) {
                pushdownPredicates(sub, r);
            }
        }
    }
}

// -----------------------------------------------------------------------
// Pass 3: Projection pruning (analysis)
// -----------------------------------------------------------------------
void Optimizer::pruneProjections(ASTNode* node, OptimizationReport& r) {
    if (!node) return;

    if (auto* sel = dynamic_cast<SelectStmt*>(node)) {
        bool hasWildcard = false;
        for (auto& col : sel->columns) {
            if (dynamic_cast<Wildcard*>(col.get())) {
                hasWildcard = true;
                break;
            }
        }

        if (!hasWildcard && !sel->columns.empty()) {
            r.add("Projection pruning: query selects " + std::to_string(sel->columns.size()) + " specific column(s)");
        }

        if (hasWildcard) {
            r.add("Projection pruning: SELECT * - all columns required, no pruning possible");
        }
    }
}

// -----------------------------------------------------------------------
// Pass 4: Dead code elimination
// -----------------------------------------------------------------------
void Optimizer::eliminateDeadCode(ASTNode* node, OptimizationReport& r) {
    if (!node) return;

    if (auto* sel = dynamic_cast<SelectStmt*>(node)) {
        if (sel->where) {
            auto* lit = dynamic_cast<Literal*>(sel->where.get());
            if (lit) {
                if (isFalsy(lit)) {
                    r.add("Dead code elimination: WHERE clause is always FALSE - query returns no rows");
                } else if (isTruthy(lit)) {
                    r.add("Dead code elimination: WHERE clause is always TRUE - removing condition");
                    sel->where.reset();
                }
            }
        }

        if (sel->having) {
            auto* lit = dynamic_cast<Literal*>(sel->having.get());
            if (lit) {
                if (isFalsy(lit)) {
                    r.add("Dead code elimination: HAVING clause is always FALSE - query returns no rows");
                } else if (isTruthy(lit)) {
                    r.add("Dead code elimination: HAVING clause is always TRUE - removing condition");
                    sel->having.reset();
                }
            }
        }
    }
}
