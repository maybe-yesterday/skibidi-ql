#pragma once
#include "ast.h"
#include <string>

class CodeGen {
public:
    // Transpile AST to SQL string
    std::string generate(const ASTNode* node);

private:
    std::string currentFromTable;
    std::string currentFromAlias;

    std::string genSelect(const SelectStmt* s);
    std::string genInsert(const InsertStmt* s);
    std::string genUpdate(const UpdateStmt* s);
    std::string genDelete(const DeleteStmt* s);
    std::string genCreate(const CreateStmt* s);
    std::string genDrop(const DropStmt* s);
    std::string genExpr(const ASTNode* node);
    std::string genFunctionCall(const FunctionCall* fc);
    std::string genWindowFunc(const WindowFunc* wf);
    std::string genOrderItem(const OrderItem& oi);

    // Special aggregate helpers
    std::string genMedian(const FunctionCall* fc, const SelectStmt* ctx);
    std::string genPercentile(const FunctionCall* fc, const SelectStmt* ctx);

    // Utility
    static std::string quoteIdent(const std::string& s);
    static std::string quoteString(const std::string& s);
    static std::string joinWith(const std::vector<std::string>& parts, const std::string& sep);
};
