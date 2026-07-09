#include "parser.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace {

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

bool wordEquals(const Token& token, const std::string& value) {
    return token.type == TokenType::IDENTIFIER &&
           lowerCopy(token.value) == lowerCopy(value);
}

bool isClauseBoundary(const Token& token) {
    if (token.type == TokenType::SEMICOLON ||
        token.type == TokenType::EOF_TOKEN ||
        token.type == TokenType::COMMA ||
        token.type == TokenType::RPAREN) {
        return true;
    }
    if (token.type == TokenType::SPLIT_BY ||
        token.type == TokenType::WITH_SEED ||
        token.type == TokenType::BATCH_SIZE ||
        token.type == TokenType::WORLD_SIZE) {
        return true;
    }
    if (token.type == TokenType::IDENTIFIER) {
        const std::string word = lowerCopy(token.value);
        return word == "seed" || word == "with" ||
               word == "features" || word == "feature" ||
               word == "label" || word == "shuffle" ||
               word == "epoch" || word == "rank" ||
               word == "worker" || word == "batch" ||
               word == "world_size" || word == "batch_size";
    }
    return false;
}

std::string tokenText(const Token& token) {
    if (token.type == TokenType::STRING_LIT) return "'" + token.value + "'";
    return token.value;
}

} // namespace

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

bool Parser::checkWord(const std::string& value) const {
    return wordEquals(current(), value);
}

bool Parser::matchWord(const std::string& value) {
    if (!checkWord(value)) return false;
    advance();
    return true;
}

const Token& Parser::expectWord(const std::string& value,
                                const std::string& msg) {
    if (!checkWord(value)) {
        throw ParseError(msg + " (got '" + current().value + "')",
                         current().line,
                         current().col);
    }
    return advance();
}

const Token& Parser::expectName(const std::string& msg) {
    if (current().type == TokenType::IDENTIFIER) return advance();
    throw ParseError(msg + " (got '" + current().value + "')",
                     current().line,
                     current().col);
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
        case TokenType::MANIFEST_DATASET:
        case TokenType::MANIFEST_SNAPSHOT:
            stmt = parseCreateSnapshot(); break;
        case TokenType::EXPORT_TORCH:
        case TokenType::SHIP_TORCH:
            stmt = parseExportTorch(); break;
        case TokenType::EXPLAIN_BATCH:
        case TokenType::SPILL_BATCH:
            stmt = parseExplainBatch(); break;
        case TokenType::MANIFEST_CONTEXT:
            stmt = parseCreateContext(); break;
        case TokenType::YEET_MEMORY:
            stmt = parseAppendMemory(); break;
        case TokenType::SPILL_CONTEXT:
            stmt = parseSpillContext(); break;
        case TokenType::VIBE_TAB:
            stmt = parseTagMemory(); break;
        case TokenType::SHOW_TABS:
            stmt = parseShowTabs(); break;
        case TokenType::SHOW_CONTEXT_SCHEMAS:
            stmt = parseShowContextSchemas(); break;
        case TokenType::SHOW_CONTEXT_OBJECTS:
            stmt = parseShowContextObjects(); break;
        case TokenType::ALIAS_TAB:
            stmt = parseAliasTab(); break;
        case TokenType::MERGE_TABS:
            stmt = parseMergeTabs(); break;
        default:
            if (checkWord("create") &&
                (wordEquals(peek(1), "dataset") ||
                 wordEquals(peek(1), "snapshot"))) {
                stmt = parseCreateSnapshot();
            } else if (checkWord("create") &&
                       wordEquals(peek(1), "context")) {
                stmt = parseCreateContext();
            } else if (checkWord("append") &&
                       wordEquals(peek(1), "memory")) {
                stmt = parseAppendMemory();
            } else if (checkWord("tag") &&
                       wordEquals(peek(1), "memory")) {
                stmt = parseTagMemory();
            } else if (checkWord("show") &&
                       wordEquals(peek(1), "tabs")) {
                stmt = parseShowTabs();
            } else if (checkWord("show") &&
                       (wordEquals(peek(1), "context-schemas") ||
                        wordEquals(peek(1), "schemas") ||
                        (wordEquals(peek(1), "context") &&
                         wordEquals(peek(2), "schemas")))) {
                stmt = parseShowContextSchemas();
            } else if (checkWord("show") &&
                       (wordEquals(peek(1), "context-objects") ||
                        (wordEquals(peek(1), "context") &&
                         wordEquals(peek(2), "objects")))) {
                stmt = parseShowContextObjects();
            } else if (checkWord("alias") &&
                       wordEquals(peek(1), "tab")) {
                stmt = parseAliasTab();
            } else if (checkWord("merge") &&
                       wordEquals(peek(1), "tabs")) {
                stmt = parseMergeTabs();
            } else if (checkWord("export") && wordEquals(peek(1), "torch")) {
                stmt = parseExportTorch();
            } else if (checkWord("explain") && wordEquals(peek(1), "batch")) {
                stmt = parseExplainBatch();
            } else if (checkWord("spill") && wordEquals(peek(1), "context")) {
                stmt = parseSpillContext();
            } else {
                throw ParseError("Expected statement keyword (slay, yeet-into, glow-up, ratio, manifest, rizz-down, manifest-snapshot, ship-torch, spill-batch, manifest-context, yeet-memory, spill-context, show-tabs, show-context-schemas, show-context-objects, alias-tab, merge-tabs, vibe-tab)", tok.line, tok.col);
            }
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
// CREATE SNAPSHOT / DATASET
// Native spelling:
//   manifest-snapshot train_v1 lowkey slay ... split-by user_id
//     with-seed 42 features (...) label clicked INT
// SQL-ish alias:
//   CREATE SNAPSHOT train_v1 AS SELECT ... SPLIT BY user_id WITH SEED 42
// -----------------------------------------------------------------------
std::unique_ptr<CreateSnapshotStmt> Parser::parseCreateSnapshot() {
    auto stmt = std::make_unique<CreateSnapshotStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::MANIFEST_DATASET) ||
        match(TokenType::MANIFEST_SNAPSHOT)) {
        // already consumed native keyword
    } else {
        expectWord("create", "Expected 'create'");
        if (matchWord("dataset") || matchWord("snapshot")) {
            // consumed plain alias
        } else {
            throw ParseError(
                "Expected 'dataset' or 'snapshot' after CREATE",
                current().line,
                current().col);
        }
    }

    stmt->name = expectName("Expected snapshot name").value;
    if (!match(TokenType::LOWKEY) && !matchWord("as")) {
        throw ParseError("Expected 'lowkey'/'as' before snapshot query",
                         current().line,
                         current().col);
    }

    stmt->source = parseSelect();

    while (!check(TokenType::SEMICOLON) &&
           !check(TokenType::EOF_TOKEN)) {
        if (match(TokenType::SPLIT_BY) || matchWord("split")) {
            (void)matchWord("by");
            if (matchWord("random")) {
                (void)matchWord("by");
            }
            stmt->splitBy = expectName("Expected split key").value;
            continue;
        }
        if (match(TokenType::WITH_SEED) || matchWord("seed")) {
            stmt->seed = parseUnsignedOption("seed");
            continue;
        }
        if (matchWord("with")) {
            expectWord("seed", "Expected 'seed' after WITH");
            stmt->seed = parseUnsignedOption("seed");
            continue;
        }
        if (matchWord("features") || matchWord("feature")) {
            stmt->features = parseFeatureSpecs();
            continue;
        }
        if (matchWord("label")) {
            stmt->label = parseLabelSpec();
            continue;
        }
        throw ParseError("Expected snapshot clause (split-by, with-seed, features, label)",
                         current().line,
                         current().col);
    }

    return stmt;
}

// -----------------------------------------------------------------------
// EXPORT TORCH / ship-torch
// -----------------------------------------------------------------------
std::unique_ptr<ExportTorchStmt> Parser::parseExportTorch() {
    auto stmt = std::make_unique<ExportTorchStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::EXPORT_TORCH) || match(TokenType::SHIP_TORCH)) {
        // native keyword
    } else {
        expectWord("export", "Expected 'export'");
        expectWord("torch", "Expected 'torch'");
    }

    stmt->dataset = expectName("Expected snapshot name").value;
    while (!check(TokenType::SEMICOLON) &&
           !check(TokenType::EOF_TOKEN)) {
        if (match(TokenType::BATCH_SIZE) || matchWord("batch_size")) {
            stmt->batchSize = parseUnsignedOption("batch-size");
        } else if (matchWord("shuffle")) {
            stmt->deterministicShuffle = true;
            (void)matchWord("deterministic");
        } else if (matchWord("deterministic")) {
            stmt->deterministicShuffle = true;
        } else if (matchWord("epoch")) {
            stmt->epoch = parseUnsignedOption("epoch");
        } else if (matchWord("rank")) {
            stmt->rank = parseUnsignedOption("rank");
        } else if (match(TokenType::WORLD_SIZE) ||
                   matchWord("world_size")) {
            stmt->worldSize = parseUnsignedOption("world-size");
        } else {
            throw ParseError("Expected export option",
                             current().line,
                             current().col);
        }
    }
    return stmt;
}

// -----------------------------------------------------------------------
// EXPLAIN BATCH / spill-batch
// -----------------------------------------------------------------------
std::unique_ptr<ExplainBatchStmt> Parser::parseExplainBatch() {
    auto stmt = std::make_unique<ExplainBatchStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::EXPLAIN_BATCH) || match(TokenType::SPILL_BATCH)) {
        // native keyword
    } else {
        expectWord("explain", "Expected 'explain'");
        expectWord("batch", "Expected 'batch'");
    }

    stmt->dataset = expectName("Expected snapshot name").value;
    while (!check(TokenType::SEMICOLON) &&
           !check(TokenType::EOF_TOKEN)) {
        if (match(TokenType::BATCH_SIZE) || matchWord("batch_size")) {
            stmt->batchSize = parseUnsignedOption("batch-size");
        } else if (matchWord("epoch")) {
            stmt->epoch = parseUnsignedOption("epoch");
        } else if (matchWord("batch")) {
            stmt->batch = parseUnsignedOption("batch");
        } else if (matchWord("rank")) {
            stmt->rank = parseUnsignedOption("rank");
        } else if (match(TokenType::WORLD_SIZE) ||
                   matchWord("world_size")) {
            stmt->worldSize = parseUnsignedOption("world-size");
        } else {
            throw ParseError("Expected explain-batch option",
                             current().line,
                             current().col);
        }
    }
    return stmt;
}

// -----------------------------------------------------------------------
// ContextQL: manifest-context / yeet-memory / spill-context
// -----------------------------------------------------------------------
std::unique_ptr<CreateContextStmt> Parser::parseCreateContext() {
    auto stmt = std::make_unique<CreateContextStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::MANIFEST_CONTEXT)) {
        // native keyword
    } else {
        expectWord("create", "Expected 'create'");
        expectWord("context", "Expected 'context'");
    }
    stmt->name = expectName("Expected context name").value;
    return stmt;
}

std::unique_ptr<AppendMemoryStmt> Parser::parseAppendMemory() {
    auto stmt = std::make_unique<AppendMemoryStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::YEET_MEMORY)) {
        // native keyword
    } else {
        expectWord("append", "Expected 'append'");
        expectWord("memory", "Expected 'memory'");
    }

    stmt->context = expectName("Expected context name").value;
    expect(TokenType::DRIP, "Expected 'drip' before memory tuple");
    expect(TokenType::LPAREN, "Expected '('");
    stmt->messageId = parseUnsignedOption("message-id");
    expect(TokenType::COMMA, "Expected ',' after message id");
    stmt->speaker =
        expect(TokenType::STRING_LIT, "Expected speaker string").value;
    expect(TokenType::COMMA, "Expected ',' after speaker");
    stmt->text =
        expect(TokenType::STRING_LIT, "Expected message text").value;
    expect(TokenType::RPAREN, "Expected ')'");
    if (match(TokenType::VIBE_TAB) || matchWord("tab")) {
        if (matchWord("auto")) {
            stmt->autoTab = true;
        } else {
            stmt->tab =
                expect(TokenType::STRING_LIT,
                       "Expected tab string or auto").value;
        }
    }
    return stmt;
}

std::unique_ptr<SpillContextStmt> Parser::parseSpillContext() {
    auto stmt = std::make_unique<SpillContextStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::SPILL_CONTEXT)) {
        // native keyword
    } else {
        expectWord("spill", "Expected 'spill'");
        expectWord("context", "Expected 'context'");
    }

    stmt->context = expectName("Expected context name").value;
    while (!check(TokenType::SEMICOLON) &&
           !check(TokenType::EOF_TOKEN)) {
        if (match(TokenType::ONLY_IF) || matchWord("query")) {
            stmt->query =
                expect(TokenType::STRING_LIT, "Expected query string").value;
        } else if (match(TokenType::TOKEN_BUDGET) ||
                   matchWord("token_budget")) {
            stmt->tokenBudget = parseUnsignedOption("token-budget");
        } else if (match(TokenType::VIBE_TAB) || matchWord("tab")) {
            stmt->tab =
                expect(TokenType::STRING_LIT, "Expected tab string").value;
        } else if (matchWord("receipts")) {
            if (matchWord("off")) {
                stmt->receipts = false;
            } else {
                (void)matchWord("on");
                stmt->receipts = true;
            }
        } else {
            throw ParseError("Expected context option",
                             current().line,
                             current().col);
        }
    }
    return stmt;
}

std::unique_ptr<TagMemoryStmt> Parser::parseTagMemory() {
    auto stmt = std::make_unique<TagMemoryStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::VIBE_TAB)) {
        // native keyword
    } else {
        expectWord("tag", "Expected 'tag'");
        expectWord("memory", "Expected 'memory'");
    }

    stmt->context = expectName("Expected context name").value;
    expectWord("message", "Expected 'message'");
    stmt->messageId = parseUnsignedOption("message-id");
    stmt->tab = expect(TokenType::STRING_LIT, "Expected tab string").value;
    return stmt;
}

std::unique_ptr<ShowTabsStmt> Parser::parseShowTabs() {
    auto stmt = std::make_unique<ShowTabsStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::SHOW_TABS)) {
        // native keyword
    } else {
        expectWord("show", "Expected 'show'");
        expectWord("tabs", "Expected 'tabs'");
    }

    stmt->context = expectName("Expected context name").value;
    return stmt;
}

std::unique_ptr<ShowContextSchemasStmt> Parser::parseShowContextSchemas() {
    auto stmt = std::make_unique<ShowContextSchemasStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::SHOW_CONTEXT_SCHEMAS)) {
        // native keyword
    } else {
        expectWord("show", "Expected 'show'");
        if (matchWord("context")) {
            expectWord("schemas", "Expected 'schemas'");
        } else if (!matchWord("context-schemas")) {
            expectWord("schemas", "Expected 'schemas'");
        }
    }

    return stmt;
}

std::unique_ptr<ShowContextObjectsStmt> Parser::parseShowContextObjects() {
    auto stmt = std::make_unique<ShowContextObjectsStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::SHOW_CONTEXT_OBJECTS)) {
        // native keyword
    } else {
        expectWord("show", "Expected 'show'");
        if (matchWord("context")) {
            expectWord("objects", "Expected 'objects'");
        } else {
            expectWord("context-objects", "Expected 'context-objects'");
        }
    }

    stmt->context = expectName("Expected context name").value;
    return stmt;
}

std::unique_ptr<AliasTabStmt> Parser::parseAliasTab() {
    auto stmt = std::make_unique<AliasTabStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::ALIAS_TAB)) {
        // native keyword
    } else {
        expectWord("alias", "Expected 'alias'");
        expectWord("tab", "Expected 'tab'");
    }

    stmt->context = expectName("Expected context name").value;
    stmt->alias = expect(TokenType::STRING_LIT,
                         "Expected alias tab string").value;
    expectWord("to", "Expected 'to'");
    stmt->target = expect(TokenType::STRING_LIT,
                          "Expected target tab string").value;
    return stmt;
}

std::unique_ptr<MergeTabsStmt> Parser::parseMergeTabs() {
    auto stmt = std::make_unique<MergeTabsStmt>();
    stmt->line = current().line;
    stmt->col = current().col;

    if (match(TokenType::MERGE_TABS)) {
        // native keyword
    } else {
        expectWord("merge", "Expected 'merge'");
        expectWord("tabs", "Expected 'tabs'");
    }

    stmt->context = expectName("Expected context name").value;
    stmt->fromTab = expect(TokenType::STRING_LIT,
                           "Expected source tab string").value;
    expectWord("into", "Expected 'into'");
    stmt->toTab = expect(TokenType::STRING_LIT,
                         "Expected target tab string").value;
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

std::vector<FeatureSpec> Parser::parseFeatureSpecs() {
    std::vector<FeatureSpec> features;
    if (match(TokenType::LPAREN)) {
        if (check(TokenType::RPAREN)) {
            advance();
            return features;
        }
        features.push_back(parseFeatureSpec());
        while (match(TokenType::COMMA)) {
            if (check(TokenType::RPAREN)) break;
            features.push_back(parseFeatureSpec());
        }
        expect(TokenType::RPAREN, "Expected ')' after feature list");
        return features;
    }

    features.push_back(parseFeatureSpec());
    return features;
}

FeatureSpec Parser::parseFeatureSpec() {
    FeatureSpec feature;
    feature.name = expectName("Expected feature column").value;

    std::ostringstream spec;
    bool first = true;
    while (!isClauseBoundary(current())) {
        if (!first) spec << " ";
        spec << tokenText(current());
        first = false;
        advance();
    }
    feature.spec = spec.str();
    return feature;
}

FeatureSpec Parser::parseLabelSpec() {
    FeatureSpec label;
    label.name = expectName("Expected label column").value;

    std::ostringstream spec;
    bool first = true;
    while (!isClauseBoundary(current())) {
        if (!first) spec << " ";
        spec << tokenText(current());
        first = false;
        advance();
    }
    label.spec = spec.str();
    return label;
}

unsigned long long Parser::parseUnsignedOption(const std::string& name) {
    const Token& token = current();
    if (token.type != TokenType::INTEGER_LIT) {
        throw ParseError("Expected integer for " + name,
                         token.line,
                         token.col);
    }
    advance();
    return static_cast<unsigned long long>(std::stoull(token.value));
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
        tok.type == TokenType::PERCENT_CHECK ||
        tok.type == TokenType::LONE_WOLF) {
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
