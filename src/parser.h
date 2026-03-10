#pragma once
#include "lexer.h"
#include "ast.h"
#include <vector>
#include <memory>
#include <stdexcept>

class ParseError : public std::runtime_error {
public:
    int line, col;
    ParseError(const std::string& msg, int l, int c)
        : std::runtime_error("Error at line " + std::to_string(l) + ", col " + std::to_string(c) + ": " + msg),
          line(l), col(c) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // Parse all statements in the token stream
    std::vector<std::unique_ptr<ASTNode>> parseAll();

    // Parse a single statement
    std::unique_ptr<ASTNode> parseStatement();

private:
    std::vector<Token> tokens;
    size_t pos;

    // Token navigation
    const Token& peek(size_t offset = 0) const;
    const Token& current() const;
    const Token& advance();
    bool check(TokenType t) const;
    bool checkAny(std::initializer_list<TokenType> types) const;
    bool match(TokenType t);
    const Token& expect(TokenType t, const std::string& msg);

    // Statement parsers
    std::unique_ptr<SelectStmt> parseSelect();
    std::unique_ptr<InsertStmt> parseInsert();
    std::unique_ptr<UpdateStmt> parseUpdate();
    std::unique_ptr<DeleteStmt> parseDelete();
    std::unique_ptr<CreateStmt> parseCreate();
    std::unique_ptr<DropStmt>   parseDrop();

    // Clause parsers
    void parseTableRef(std::string& table, std::string& alias);
    JoinClause parseJoin();
    std::vector<std::unique_ptr<ASTNode>> parseSelectList();
    std::unique_ptr<ASTNode> parseSelectItem();
    std::vector<std::unique_ptr<ASTNode>> parseColumnList();
    std::vector<OrderItem> parseOrderList();
    ColumnDef parseColumnDef();

    // Expression parsers (recursive descent)
    std::unique_ptr<ASTNode> parseExpr();
    std::unique_ptr<ASTNode> parseOrExpr();
    std::unique_ptr<ASTNode> parseAndExpr();
    std::unique_ptr<ASTNode> parseNotExpr();
    std::unique_ptr<ASTNode> parseCompareExpr();
    std::unique_ptr<ASTNode> parseAddExpr();
    std::unique_ptr<ASTNode> parseMulExpr();
    std::unique_ptr<ASTNode> parseUnaryExpr();
    std::unique_ptr<ASTNode> parsePrimaryExpr();
    std::unique_ptr<ASTNode> parseFunctionCall(const std::string& name, int l, int c);
    std::unique_ptr<ASTNode> parseEraExpr(int l, int c);
};
