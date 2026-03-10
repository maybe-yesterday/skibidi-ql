#include "parser.h"
#include <cassert>
#include <stdexcept>

// -----------------------------------------------------------------------
// Constructor / helpers
// -----------------------------------------------------------------------
Parser::Parser(std::vector<Token> toks) : tokens(std::move(toks)), pos(0) {}

const Token& Parser::peek(size_t offset) const {
    size_t idx = pos + offset;
    if (idx >= tokens.size()) return tokens.back(); // EOF
    return tokens[idx];
}

const Token& Parser::current() const {
    return peek(0);
}

const Token& Parser::advance() {
    const Token& t = tokens[pos];
    if (pos + 1 < tokens.size()) pos++;
    return t;
}

bool Parser::check(TokenType t) const {
    return current().type == t;
}

bool Parser::checkAny(std::initializer_list<TokenType> types) const {
    for (auto t : types) if (check(t)) return true;
    return false;
}

bool Parser::match(TokenType t) {
    if (check(t)) { advance(); return true; }
    return false;
}

const Token& Parser::expect(TokenType t, const std::string& msg) {
    if (!check(t)) {
        throw ParseError(msg + " (got '" + current().value + "')", current().line, current().col);
    }
    return advance();
}

// -----------------------------------------------------------------------
// Top-level parse
// -----------------------------------------------------------------------
std::vector<std::unique_ptr<ASTNode>> Parser::parseAll() {
    std::vector<std::unique_ptr<ASTNode>> stmts;
    while (!check(TokenType::EOF_TOKEN)) {
        stmts.push_back(parseStatement());
        // consume optional trailing semicolons
        while (match(TokenType::SEMICOLON)) {}
    }
    return stmts;
}

std::unique_ptr<ASTNode> Parser::parseStatement() {
    const Token& tok = current();

    std::unique_ptr<ASTNode> stmt;
    switch (tok.type) {
        case TokenType::SLAY:      stmt = parseSelect(); break;
        case TokenType::YEET_INTO: stmt = parseInsert(); break;
        case TokenType::GLOW_UP:   stmt = parseUpdate(); break;
        case TokenType::RATIO:     stmt = parseDelete(); break;
        case TokenType::MANIFEST:  stmt = parseCreate(); break;
        case TokenType::RIZZ_DOWN: stmt = parseDrop();   break;
        default:
            throw ParseError("Expected statement keyword (slay, yeet-into, glow-up, ratio, manifest, rizz-down)", tok.line, tok.col);
    }

    // Consume optional semicolon
    match(TokenType::SEMICOLON);
    return stmt;
}

// -----------------------------------------------------------------------
// SELECT
// -----------------------------------------------------------------------
std::unique_ptr<SelectStmt> Parser::parseSelect() {
    auto stmt = std::make_unique<SelectStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    expect(TokenType::SLAY, "Expected 'slay'");

    // Optional DISTINCT
    if (match(TokenType::UNIQUE_FR)) {
        stmt->distinct = true;
    }

    // Select list
    stmt->columns = parseSelectList();

    // FROM (no-cap)
    expect(TokenType::NO_CAP, "Expected 'no-cap' (FROM)");
    parseTableRef(stmt->fromTable, stmt->fromAlias);

    // JOINs
    while (checkAny({TokenType::LINK_UP, TokenType::LEFT_LINK_UP, TokenType::MID_LINK_UP})) {
        stmt->joins.push_back(parseJoin());
    }

    // WHERE (only-if)
    if (match(TokenType::ONLY_IF)) {
        stmt->where = parseExpr();
    }

    // GROUP BY (vibe-check)
    if (match(TokenType::VIBE_CHECK)) {
        stmt->groupBy = parseColumnList();
    }

    // HAVING (bussin-only)
    if (match(TokenType::BUSSIN_ONLY)) {
        stmt->having = parseExpr();
    }

    // ORDER BY (hits-different)
    if (match(TokenType::HITS_DIFFERENT)) {
        stmt->orderBy = parseOrderList();
    }

    // LIMIT (cap-at)
    if (match(TokenType::CAP_AT)) {
        stmt->limit = parseExpr();
    }

    // OFFSET (skip)
    if (match(TokenType::SKIP)) {
        stmt->offset = parseExpr();
    }

    return stmt;
}

// -----------------------------------------------------------------------
// INSERT
// -----------------------------------------------------------------------
std::unique_ptr<InsertStmt> Parser::parseInsert() {
    auto stmt = std::make_unique<InsertStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    expect(TokenType::YEET_INTO, "Expected 'yeet-into'");
    stmt->table = expect(TokenType::IDENTIFIER, "Expected table name").value;

    // Optional column list
    if (match(TokenType::LPAREN)) {
        stmt->columns.push_back(expect(TokenType::IDENTIFIER, "Expected column name").value);
        while (match(TokenType::COMMA)) {
            stmt->columns.push_back(expect(TokenType::IDENTIFIER, "Expected column name").value);
        }
        expect(TokenType::RPAREN, "Expected ')'");
    }

    // VALUES (drip)
    expect(TokenType::DRIP, "Expected 'drip' (VALUES)");

    // Parse one or more value rows
    do {
        expect(TokenType::LPAREN, "Expected '('");
        std::vector<std::unique_ptr<ASTNode>> row;
        row.push_back(parseExpr());
        while (match(TokenType::COMMA)) {
            row.push_back(parseExpr());
        }
        expect(TokenType::RPAREN, "Expected ')'");
        stmt->valueRows.push_back(std::move(row));
    } while (match(TokenType::COMMA));

    return stmt;
}

// -----------------------------------------------------------------------
// UPDATE
// -----------------------------------------------------------------------
std::unique_ptr<UpdateStmt> Parser::parseUpdate() {
    auto stmt = std::make_unique<UpdateStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    expect(TokenType::GLOW_UP, "Expected 'glow-up'");
    stmt->table = expect(TokenType::IDENTIFIER, "Expected table name").value;
    expect(TokenType::BE_LIKE, "Expected 'be-like' (SET)");

    // Parse assignments
    auto parseAssignment = [&]() {
        Assignment a;
        a.column = expect(TokenType::IDENTIFIER, "Expected column name").value;
        expect(TokenType::EQ, "Expected '='");
        a.value = parseExpr();
        stmt->sets.push_back(std::move(a));
    };

    parseAssignment();
    while (match(TokenType::COMMA)) {
        parseAssignment();
    }

    // Optional WHERE
    if (match(TokenType::ONLY_IF)) {
        stmt->where = parseExpr();
    }

    return stmt;
}

// -----------------------------------------------------------------------
// DELETE
// -----------------------------------------------------------------------
std::unique_ptr<DeleteStmt> Parser::parseDelete() {
    auto stmt = std::make_unique<DeleteStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    expect(TokenType::RATIO, "Expected 'ratio'");
    stmt->table = expect(TokenType::IDENTIFIER, "Expected table name").value;

    if (match(TokenType::ONLY_IF)) {
        stmt->where = parseExpr();
    }

    return stmt;
}

// -----------------------------------------------------------------------
// CREATE TABLE
// -----------------------------------------------------------------------
std::unique_ptr<CreateStmt> Parser::parseCreate() {
    auto stmt = std::make_unique<CreateStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    expect(TokenType::MANIFEST, "Expected 'manifest'");
    stmt->table = expect(TokenType::IDENTIFIER, "Expected table name").value;
    expect(TokenType::LPAREN, "Expected '('");

    stmt->columns.push_back(parseColumnDef());
    while (match(TokenType::COMMA)) {
        // Check if next is IDENTIFIER (column def) vs end
        if (check(TokenType::RPAREN)) break;
        stmt->columns.push_back(parseColumnDef());
    }

    expect(TokenType::RPAREN, "Expected ')'");
    return stmt;
}

// -----------------------------------------------------------------------
// DROP TABLE
// -----------------------------------------------------------------------
std::unique_ptr<DropStmt> Parser::parseDrop() {
    auto stmt = std::make_unique<DropStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    expect(TokenType::RIZZ_DOWN, "Expected 'rizz-down'");
    stmt->table = expect(TokenType::IDENTIFIER, "Expected table name").value;
    return stmt;
}

// -----------------------------------------------------------------------
// Table ref: IDENTIFIER ['lowkey' IDENTIFIER]
// -----------------------------------------------------------------------
void Parser::parseTableRef(std::string& table, std::string& alias) {
    table = expect(TokenType::IDENTIFIER, "Expected table name").value;
    alias.clear();
    if (match(TokenType::LOWKEY)) {
        alias = expect(TokenType::IDENTIFIER, "Expected alias name").value;
    }
}

// -----------------------------------------------------------------------
// JOIN clause
// -----------------------------------------------------------------------
JoinClause Parser::parseJoin() {
    JoinClause jc;

    if (match(TokenType::LEFT_LINK_UP)) {
        jc.type = JoinType::LEFT;
    } else if (match(TokenType::MID_LINK_UP)) {
        jc.type = JoinType::INNER;
    } else {
        expect(TokenType::LINK_UP, "Expected join keyword");
        jc.type = JoinType::INNER;
    }

    parseTableRef(jc.table, jc.alias);
    expect(TokenType::FR_FR, "Expected 'fr-fr' (ON)");
    jc.condition = parseExpr();
    return jc;
}

// -----------------------------------------------------------------------
// Select list
// -----------------------------------------------------------------------
std::vector<std::unique_ptr<ASTNode>> Parser::parseSelectList() {
    std::vector<std::unique_ptr<ASTNode>> list;

    // Check for bare *
    if (check(TokenType::STAR)) {
        int l = current().line, c = current().col;
        advance();
        auto w = std::make_unique<Wildcard>();
        w->line = l; w->col = c;
        list.push_back(std::move(w));
        return list;
    }

    list.push_back(parseSelectItem());
    while (match(TokenType::COMMA)) {
        list.push_back(parseSelectItem());
    }
    return list;
}

std::unique_ptr<ASTNode> Parser::parseSelectItem() {
    auto expr = parseExpr();

    // Optional alias (lowkey)
    if (match(TokenType::LOWKEY)) {
        std::string alias = expect(TokenType::IDENTIFIER, "Expected alias name").value;
        // Attach alias to the expression node
        if (auto* cr = dynamic_cast<ColumnRef*>(expr.get())) {
            cr->alias = alias;
        } else if (auto* fc = dynamic_cast<FunctionCall*>(expr.get())) {
            fc->alias = alias;
        } else if (auto* bo = dynamic_cast<BinaryOp*>(expr.get())) {
            bo->alias = alias;
        } else if (auto* uo = dynamic_cast<UnaryOp*>(expr.get())) {
            uo->alias = alias;
        } else if (auto* wf = dynamic_cast<WindowFunc*>(expr.get())) {
            wf->alias = alias;
        } else if (auto* wc = dynamic_cast<Wildcard*>(expr.get())) {
            wc->alias = alias;
        }
    }
    return expr;
}

// -----------------------------------------------------------------------
// Column list (for GROUP BY, etc.) - just a comma-separated list of exprs
// -----------------------------------------------------------------------
std::vector<std::unique_ptr<ASTNode>> Parser::parseColumnList() {
    std::vector<std::unique_ptr<ASTNode>> list;
    list.push_back(parseExpr());
    while (match(TokenType::COMMA)) {
        list.push_back(parseExpr());
    }
    return list;
}

// -----------------------------------------------------------------------
// ORDER list: expr [up-only|down-bad] (',' expr [up-only|down-bad])*
// -----------------------------------------------------------------------
std::vector<OrderItem> Parser::parseOrderList() {
    std::vector<OrderItem> list;

    auto parseOne = [&]() {
        OrderItem oi;
        oi.expr = parseExpr();
        if (match(TokenType::DOWN_BAD)) {
            oi.asc = false;
        } else {
            match(TokenType::UP_ONLY); // consume optional ASC
            oi.asc = true;
        }
        return oi;
    };

    list.push_back(parseOne());
    while (match(TokenType::COMMA)) {
        list.push_back(parseOne());
    }
    return list;
}

// -----------------------------------------------------------------------
// Column definition for CREATE TABLE
// -----------------------------------------------------------------------
ColumnDef Parser::parseColumnDef() {
    ColumnDef def;
    def.name = expect(TokenType::IDENTIFIER, "Expected column name").value;

    // Type name - could be INTEGER, TEXT, REAL, BLOB (treated as identifiers)
    const Token& typeTok = current();
    if (typeTok.type == TokenType::IDENTIFIER) {
        def.type = typeTok.value;
        advance();
    } else {
        throw ParseError("Expected type name (INTEGER, TEXT, REAL, BLOB)", typeTok.line, typeTok.col);
    }

    // Constraints
    while (true) {
        if (match(TokenType::MAIN_CHARACTER)) {
            def.primary_key = true;
        } else if (check(TokenType::NO_CAP_NOT)) {
            // no-cap-not ghosted = NOT NULL
            advance(); // consume no-cap-not
            expect(TokenType::GHOSTED, "Expected 'ghosted' (NULL) after 'no-cap-not'");
            def.not_null = true;
        } else if (match(TokenType::SIDE_CHARACTER)) {
            // side-character references table(col)
            // "references" is an identifier token
            const Token& refTok = current();
            if (refTok.type != TokenType::IDENTIFIER || refTok.value != "references") {
                throw ParseError("Expected 'references' after 'side-character'", refTok.line, refTok.col);
            }
            advance();
            def.fk_table = expect(TokenType::IDENTIFIER, "Expected foreign table name").value;
            expect(TokenType::LPAREN, "Expected '('");
            def.fk_col = expect(TokenType::IDENTIFIER, "Expected foreign column name").value;
            expect(TokenType::RPAREN, "Expected ')'");
        } else {
            break;
        }
    }

    return def;
}

// -----------------------------------------------------------------------
// Expression parsing
// -----------------------------------------------------------------------
std::unique_ptr<ASTNode> Parser::parseExpr() {
    return parseOrExpr();
}

std::unique_ptr<ASTNode> Parser::parseOrExpr() {
    auto left = parseAndExpr();

    while (check(TokenType::OR_NAH)) {
        int l = current().line, c = current().col;
        advance();
        auto right = parseAndExpr();
        auto node = std::make_unique<BinaryOp>();
        node->op = "OR";
        node->left = std::move(left);
        node->right = std::move(right);
        node->line = l; node->col = c;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parseAndExpr() {
    auto left = parseNotExpr();

    while (check(TokenType::PLUS)) {
        int l = current().line, c = current().col;
        advance();
        auto right = parseNotExpr();
        auto node = std::make_unique<BinaryOp>();
        node->op = "AND";
        node->left = std::move(left);
        node->right = std::move(right);
        node->line = l; node->col = c;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parseNotExpr() {
    if (check(TokenType::NO_CAP_NOT)) {
        int l = current().line, c = current().col;
        advance();
        auto operand = parseNotExpr();
        auto node = std::make_unique<UnaryOp>();
        node->op = "NOT";
        node->operand = std::move(operand);
        node->line = l; node->col = c;
        return node;
    }
    return parseCompareExpr();
}

std::unique_ptr<ASTNode> Parser::parseCompareExpr() {
    auto left = parseAddExpr();

    // Check for comparison operators
    TokenType tt = current().type;
    if (tt == TokenType::EQ || tt == TokenType::NEQ ||
        tt == TokenType::LT || tt == TokenType::GT ||
        tt == TokenType::LTE || tt == TokenType::GTE) {
        int l = current().line, c = current().col;
        std::string op = current().value;
        advance();
        auto right = parseAddExpr();
        auto node = std::make_unique<BinaryOp>();
        node->op = op;
        node->left = std::move(left);
        node->right = std::move(right);
        node->line = l; node->col = c;
        return node;
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseAddExpr() {
    auto left = parseMulExpr();

    while (check(TokenType::PLUS_OP) || check(TokenType::MINUS) || check(TokenType::CONCAT)) {
        int l = current().line, c = current().col;
        std::string op = current().value;
        advance();
        auto right = parseMulExpr();
        auto node = std::make_unique<BinaryOp>();
        node->op = op;
        node->left = std::move(left);
        node->right = std::move(right);
        node->line = l; node->col = c;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parseMulExpr() {
    auto left = parseUnaryExpr();

    while (check(TokenType::STAR) || check(TokenType::SLASH)) {
        int l = current().line, c = current().col;
        std::string op = current().value;
        advance();
        auto right = parseUnaryExpr();
        auto node = std::make_unique<BinaryOp>();
        node->op = op;
        node->left = std::move(left);
        node->right = std::move(right);
        node->line = l; node->col = c;
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parseUnaryExpr() {
    if (check(TokenType::MINUS)) {
        int l = current().line, c = current().col;
        advance();
        auto operand = parseUnaryExpr();
        auto node = std::make_unique<UnaryOp>();
        node->op = "-";
        node->operand = std::move(operand);
        node->line = l; node->col = c;
        return node;
    }
    return parsePrimaryExpr();
}

std::unique_ptr<ASTNode> Parser::parsePrimaryExpr() {
    const Token& tok = current();

    // Integer literal
    if (tok.type == TokenType::INTEGER_LIT) {
        advance();
        return Literal::makeInt(std::stoll(tok.value), tok.line, tok.col);
    }

    // Float literal
    if (tok.type == TokenType::FLOAT_LIT) {
        advance();
        return Literal::makeFloat(std::stod(tok.value), tok.line, tok.col);
    }

    // String literal
    if (tok.type == TokenType::STRING_LIT) {
        advance();
        return Literal::makeString(tok.value, tok.line, tok.col);
    }

    // NULL (ghosted)
    if (tok.type == TokenType::GHOSTED) {
        advance();
        return Literal::makeNull(tok.line, tok.col);
    }

    // Parenthesized expression or subquery
    if (tok.type == TokenType::LPAREN) {
        advance();
        // Check if it's a subquery
        if (check(TokenType::SLAY)) {
            auto sub = parseSelect();
            expect(TokenType::RPAREN, "Expected ')'");
            return sub;
        }
        auto expr = parseExpr();
        expect(TokenType::RPAREN, "Expected ')'");
        return expr;
    }

    // Wildcard * (used in function args like headcount(*))
    if (tok.type == TokenType::STAR) {
        advance();
        auto w = std::make_unique<Wildcard>();
        w->line = tok.line; w->col = tok.col;
        return w;
    }

    // era (window function)
    if (tok.type == TokenType::ERA) {
        advance();
        return parseEraExpr(tok.line, tok.col);
    }

    // Function keywords that are built-in aggregates
    if (tok.type == TokenType::HEADCOUNT || tok.type == TokenType::STACK ||
        tok.type == TokenType::MID       || tok.type == TokenType::GOAT  ||
        tok.type == TokenType::L_FUNC    || tok.type == TokenType::BIGGEST_W ||
        tok.type == TokenType::BIGGEST_L || tok.type == TokenType::MID_FR ||
        tok.type == TokenType::PERCENT_CHECK) {
        std::string fname = tok.value;
        advance();
        return parseFunctionCall(fname, tok.line, tok.col);
    }

    // Identifier (or qualified column ref)
    if (tok.type == TokenType::IDENTIFIER) {
        advance();
        std::string name = tok.value;

        // Check for table.column
        if (check(TokenType::DOT)) {
            advance(); // consume '.'
            std::string col;
            if (check(TokenType::STAR)) {
                // table.*
                advance();
                auto w = std::make_unique<Wildcard>();
                w->table = name;
                w->line = tok.line; w->col = tok.col;
                return w;
            }
            col = expect(TokenType::IDENTIFIER, "Expected column name after '.'").value;

            // Could be a function call: schema.function()
            if (check(TokenType::LPAREN)) {
                return parseFunctionCall(name + "." + col, tok.line, tok.col);
            }

            auto cr = std::make_unique<ColumnRef>();
            cr->table = name;
            cr->column = col;
            cr->line = tok.line; cr->col = tok.col;
            return cr;
        }

        // Check for function call
        if (check(TokenType::LPAREN)) {
            return parseFunctionCall(name, tok.line, tok.col);
        }

        // Plain column ref
        auto cr = std::make_unique<ColumnRef>();
        cr->column = name;
        cr->line = tok.line; cr->col = tok.col;
        return cr;
    }

    throw ParseError("Expected expression (got '" + tok.value + "')", tok.line, tok.col);
}

// -----------------------------------------------------------------------
// Function call
// -----------------------------------------------------------------------
std::unique_ptr<ASTNode> Parser::parseFunctionCall(const std::string& name, int l, int c) {
    auto fc = std::make_unique<FunctionCall>();
    fc->name = name;
    fc->line = l; fc->col = c;

    expect(TokenType::LPAREN, "Expected '(' after function name");

    // headcount(*) special case
    if (name == "headcount" && check(TokenType::STAR)) {
        advance(); // consume *
        auto w = std::make_unique<Wildcard>();
        w->line = l; w->col = c;
        fc->args.push_back(std::move(w));
        expect(TokenType::RPAREN, "Expected ')'");
        return fc;
    }

    // headcount(unique-fr col)
    if (name == "headcount" && check(TokenType::UNIQUE_FR)) {
        advance();
        fc->distinct = true;
        fc->args.push_back(parseExpr());
        expect(TokenType::RPAREN, "Expected ')'");
        return fc;
    }

    // Empty arg list
    if (check(TokenType::RPAREN)) {
        advance();
        return fc;
    }

    fc->args.push_back(parseExpr());
    while (match(TokenType::COMMA)) {
        fc->args.push_back(parseExpr());
    }

    expect(TokenType::RPAREN, "Expected ')'");
    return fc;
}

// -----------------------------------------------------------------------
// era (window function) parsing
// era [split-by col, col] hits-different col [up-only|down-bad]
// -----------------------------------------------------------------------
std::unique_ptr<ASTNode> Parser::parseEraExpr(int l, int c) {
    auto wf = std::make_unique<WindowFunc>();
    wf->funcName = "RANK";
    wf->line = l; wf->col = c;

    // Optional split-by (PARTITION BY)
    if (match(TokenType::SPLIT_BY)) {
        wf->partition_by = parseColumnList();
    }

    // hits-different (ORDER BY) - required for era
    expect(TokenType::HITS_DIFFERENT, "Expected 'hits-different' (ORDER BY) in era expression");
    wf->order_by = parseOrderList();

    return wf;
}
