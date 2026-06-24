#pragma once
#include "ast.h"
#include "metadata.h"
#include <string>
#include <vector>
#include <memory>

struct OptimizationReport {
    std::vector<std::string> notes;
    void add(const std::string& note) { notes.push_back(note); }
};

class Optimizer {
public:
    explicit Optimizer(bool verbose = false, const Catalog* catalog = nullptr);

    // Run all passes, return (possibly modified) AST
    std::unique_ptr<ASTNode> optimize(std::unique_ptr<ASTNode> ast, OptimizationReport& report);

private:
    bool verbose_;
    const Catalog* catalog_;

    // Pass 1: Constant folding
    std::unique_ptr<ASTNode> foldConstants(std::unique_ptr<ASTNode> node, OptimizationReport& r);
    std::unique_ptr<ASTNode> tryFoldExpr(ASTNode* node, OptimizationReport& r);

    // Pass 2: Metadata-aware rewrites
    void applyMetadataOptimizations(ASTNode* node, OptimizationReport& r);

    // Pass 3: Predicate pushdown
    void pushdownPredicates(ASTNode* node, OptimizationReport& r);

    // Pass 4: Projection pruning
    void pruneProjections(ASTNode* node, OptimizationReport& r);

    // Pass 5: Dead code elimination
    void eliminateDeadCode(ASTNode* node, OptimizationReport& r);

    // Helpers
    bool isConstant(const ASTNode* node) const;
    bool isTruthy(const Literal* lit) const;
    bool isFalsy(const Literal* lit) const;
    const ColumnMeta* resolveBaseColumn(const SelectStmt* select,
                                        const ColumnRef* column) const;
    bool projectsPrimaryKey(const SelectStmt* select) const;
    bool wherePinsPrimaryKey(const SelectStmt* select,
                             const ASTNode* expression) const;
    bool hasSetProducingExpression(const SelectStmt* select) const;
    void rewriteNonNullCounts(SelectStmt* select,
                              ASTNode* expression,
                              OptimizationReport& r);
};
