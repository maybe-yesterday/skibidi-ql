// Tests for Lexer::tokenize()
// Covers: all keyword tokens, literals, operators, punctuation,
//         comments, error cases, line/col tracking, edge cases.
#include "test_framework.h"
#include "lexer.h"
#include <vector>
#include <stdexcept>

// Helper: lex and return the token vector.
static std::vector<Token> lex(const std::string& src) {
    return Lexer(src).tokenize();
}

// Helper: lex and return the first token type.
static TokenType firstType(const std::string& src) {
    return lex(src)[0].type;
}

// -----------------------------------------------------------------------
// Edge cases – empty / whitespace
// -----------------------------------------------------------------------
TEST(empty_string_produces_eof) {
    auto toks = lex("");
    ASSERT_EQ(toks.size(), (size_t)1);
    ASSERT_TRUE(toks[0].type == TokenType::EOF_TOKEN);
}

TEST(whitespace_only_produces_eof) {
    auto toks = lex("   \t\n  ");
    ASSERT_EQ(toks.size(), (size_t)1);
    ASSERT_TRUE(toks[0].type == TokenType::EOF_TOKEN);
}

// -----------------------------------------------------------------------
// Single-word keywords
// -----------------------------------------------------------------------
TEST(keyword_slay) {
    ASSERT_TRUE(firstType("slay") == TokenType::SLAY);
}
TEST(keyword_lowkey) {
    ASSERT_TRUE(firstType("lowkey") == TokenType::LOWKEY);
}
TEST(keyword_ghosted) {
    ASSERT_TRUE(firstType("ghosted") == TokenType::GHOSTED);
}
TEST(keyword_drip) {
    ASSERT_TRUE(firstType("drip") == TokenType::DRIP);
}
TEST(keyword_ratio) {
    ASSERT_TRUE(firstType("ratio") == TokenType::RATIO);
}
TEST(keyword_manifest) {
    ASSERT_TRUE(firstType("manifest") == TokenType::MANIFEST);
}
TEST(keyword_headcount) {
    ASSERT_TRUE(firstType("headcount") == TokenType::HEADCOUNT);
}
TEST(keyword_stack) {
    ASSERT_TRUE(firstType("stack") == TokenType::STACK);
}
TEST(keyword_mid) {
    ASSERT_TRUE(firstType("mid") == TokenType::MID);
}
TEST(keyword_goat) {
    ASSERT_TRUE(firstType("goat") == TokenType::GOAT);
}
TEST(keyword_era) {
    ASSERT_TRUE(firstType("era") == TokenType::ERA);
}
TEST(keyword_skip) {
    ASSERT_TRUE(firstType("skip") == TokenType::SKIP);
}
TEST(keyword_plus) {
    ASSERT_TRUE(firstType("plus") == TokenType::PLUS);
}

// -----------------------------------------------------------------------
// Hyphenated keywords
// -----------------------------------------------------------------------
TEST(keyword_no_cap) {
    ASSERT_TRUE(firstType("no-cap") == TokenType::NO_CAP);
}
TEST(keyword_no_cap_not) {
    // Longest match: no-cap-not beats no-cap
    ASSERT_TRUE(firstType("no-cap-not") == TokenType::NO_CAP_NOT);
}
TEST(keyword_only_if) {
    ASSERT_TRUE(firstType("only-if") == TokenType::ONLY_IF);
}
TEST(keyword_link_up) {
    ASSERT_TRUE(firstType("link-up") == TokenType::LINK_UP);
}
TEST(keyword_left_link_up) {
    ASSERT_TRUE(firstType("left-link-up") == TokenType::LEFT_LINK_UP);
}
TEST(keyword_mid_link_up) {
    ASSERT_TRUE(firstType("mid-link-up") == TokenType::MID_LINK_UP);
}
TEST(keyword_fr_fr) {
    ASSERT_TRUE(firstType("fr-fr") == TokenType::FR_FR);
}
TEST(keyword_vibe_check) {
    ASSERT_TRUE(firstType("vibe-check") == TokenType::VIBE_CHECK);
}
TEST(keyword_hits_different) {
    ASSERT_TRUE(firstType("hits-different") == TokenType::HITS_DIFFERENT);
}
TEST(keyword_bussin_only) {
    ASSERT_TRUE(firstType("bussin-only") == TokenType::BUSSIN_ONLY);
}
TEST(keyword_yeet_into) {
    ASSERT_TRUE(firstType("yeet-into") == TokenType::YEET_INTO);
}
TEST(keyword_glow_up) {
    ASSERT_TRUE(firstType("glow-up") == TokenType::GLOW_UP);
}
TEST(keyword_be_like) {
    ASSERT_TRUE(firstType("be-like") == TokenType::BE_LIKE);
}
TEST(keyword_rizz_down) {
    ASSERT_TRUE(firstType("rizz-down") == TokenType::RIZZ_DOWN);
}
TEST(keyword_main_character) {
    ASSERT_TRUE(firstType("main-character") == TokenType::MAIN_CHARACTER);
}
TEST(keyword_side_character) {
    ASSERT_TRUE(firstType("side-character") == TokenType::SIDE_CHARACTER);
}
TEST(keyword_unique_fr) {
    ASSERT_TRUE(firstType("unique-fr") == TokenType::UNIQUE_FR);
}
TEST(keyword_cap_at) {
    ASSERT_TRUE(firstType("cap-at") == TokenType::CAP_AT);
}
TEST(keyword_up_only) {
    ASSERT_TRUE(firstType("up-only") == TokenType::UP_ONLY);
}
TEST(keyword_down_bad) {
    ASSERT_TRUE(firstType("down-bad") == TokenType::DOWN_BAD);
}
TEST(keyword_or_nah) {
    ASSERT_TRUE(firstType("or-nah") == TokenType::OR_NAH);
}
TEST(keyword_biggest_W) {
    ASSERT_TRUE(firstType("biggest-W") == TokenType::BIGGEST_W);
}
TEST(keyword_biggest_L) {
    ASSERT_TRUE(firstType("biggest-L") == TokenType::BIGGEST_L);
}
TEST(keyword_mid_fr) {
    ASSERT_TRUE(firstType("mid-fr") == TokenType::MID_FR);
}
TEST(keyword_percent_check) {
    ASSERT_TRUE(firstType("percent-check") == TokenType::PERCENT_CHECK);
}
TEST(keyword_split_by) {
    ASSERT_TRUE(firstType("split-by") == TokenType::SPLIT_BY);
}

// -----------------------------------------------------------------------
// Special: single "L" is L_FUNC (MIN)
// -----------------------------------------------------------------------
TEST(single_L_is_L_FUNC) {
    auto toks = lex("L");
    ASSERT_TRUE(toks[0].type == TokenType::L_FUNC);
    ASSERT_EQ(toks[0].value, std::string("L"));
}

TEST(L_in_expression_is_L_FUNC) {
    auto toks = lex("L(price)");
    ASSERT_TRUE(toks[0].type == TokenType::L_FUNC);
}

// -----------------------------------------------------------------------
// Identifier (not a keyword)
// -----------------------------------------------------------------------
TEST(plain_identifier) {
    auto toks = lex("myTable");
    ASSERT_TRUE(toks[0].type == TokenType::IDENTIFIER);
    ASSERT_EQ(toks[0].value, std::string("myTable"));
}

TEST(identifier_with_underscore) {
    auto toks = lex("user_id");
    ASSERT_TRUE(toks[0].type == TokenType::IDENTIFIER);
    ASSERT_EQ(toks[0].value, std::string("user_id"));
}

TEST(identifier_starts_with_underscore) {
    auto toks = lex("_col");
    ASSERT_TRUE(toks[0].type == TokenType::IDENTIFIER);
}

TEST(identifier_with_digits) {
    auto toks = lex("col2");
    ASSERT_TRUE(toks[0].type == TokenType::IDENTIFIER);
}

// Non-keyword hyphenated word: "foo-bar" → IDENTIFIER("foo") then operators
TEST(non_keyword_hyphenated_splits_at_hyphen) {
    auto toks = lex("foo-bar");
    // "foo" is IDENTIFIER, then "-" is MINUS, then "bar" is IDENTIFIER
    ASSERT_TRUE(toks[0].type == TokenType::IDENTIFIER);
    ASSERT_EQ(toks[0].value, std::string("foo"));
    ASSERT_TRUE(toks[1].type == TokenType::MINUS);
    ASSERT_TRUE(toks[2].type == TokenType::IDENTIFIER);
    ASSERT_EQ(toks[2].value, std::string("bar"));
}

// -----------------------------------------------------------------------
// Integer literals
// -----------------------------------------------------------------------
TEST(integer_literal_simple) {
    auto toks = lex("42");
    ASSERT_TRUE(toks[0].type == TokenType::INTEGER_LIT);
    ASSERT_EQ(toks[0].value, std::string("42"));
}

TEST(integer_literal_zero) {
    auto toks = lex("0");
    ASSERT_TRUE(toks[0].type == TokenType::INTEGER_LIT);
    ASSERT_EQ(toks[0].value, std::string("0"));
}

TEST(integer_literal_large) {
    auto toks = lex("1234567890");
    ASSERT_TRUE(toks[0].type == TokenType::INTEGER_LIT);
    ASSERT_EQ(toks[0].value, std::string("1234567890"));
}

// -----------------------------------------------------------------------
// Float literals
// -----------------------------------------------------------------------
TEST(float_literal_simple) {
    auto toks = lex("3.14");
    ASSERT_TRUE(toks[0].type == TokenType::FLOAT_LIT);
    ASSERT_EQ(toks[0].value, std::string("3.14"));
}

TEST(float_literal_zero_dot) {
    auto toks = lex("0.5");
    ASSERT_TRUE(toks[0].type == TokenType::FLOAT_LIT);
}

TEST(float_literal_scientific_e) {
    auto toks = lex("1e5");
    ASSERT_TRUE(toks[0].type == TokenType::FLOAT_LIT);
    ASSERT_EQ(toks[0].value, std::string("1e5"));
}

TEST(float_literal_scientific_E) {
    auto toks = lex("2E10");
    ASSERT_TRUE(toks[0].type == TokenType::FLOAT_LIT);
}

TEST(float_literal_scientific_plus_exp) {
    auto toks = lex("1.5e+3");
    ASSERT_TRUE(toks[0].type == TokenType::FLOAT_LIT);
    ASSERT_EQ(toks[0].value, std::string("1.5e+3"));
}

TEST(float_literal_scientific_minus_exp) {
    auto toks = lex("2.0e-4");
    ASSERT_TRUE(toks[0].type == TokenType::FLOAT_LIT);
    ASSERT_EQ(toks[0].value, std::string("2.0e-4"));
}

TEST(invalid_scientific_notation_throws) {
    ASSERT_THROW(lex("1e"), LexerError);
}

// -----------------------------------------------------------------------
// String literals
// -----------------------------------------------------------------------
TEST(string_literal_simple) {
    auto toks = lex("'hello'");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value, std::string("hello"));
}

TEST(string_literal_empty) {
    auto toks = lex("''");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value, std::string(""));
}

TEST(string_literal_double_quote_escape) {
    // '' inside a string becomes a single quote
    auto toks = lex("'it''s'");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value, std::string("it's"));
}

TEST(string_literal_backslash_n) {
    auto toks = lex("'foo\\nbar'");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value[3], '\n');
}

TEST(string_literal_backslash_t) {
    auto toks = lex("'foo\\tbar'");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value[3], '\t');
}

TEST(string_literal_backslash_r) {
    auto toks = lex("'foo\\rbar'");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value[3], '\r');
}

TEST(string_literal_escaped_backslash) {
    auto toks = lex("'foo\\\\bar'");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value[3], '\\');
}

TEST(string_literal_escaped_quote) {
    auto toks = lex("'foo\\'bar'");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value[3], '\'');
}

TEST(string_literal_with_spaces) {
    auto toks = lex("'hello world'");
    ASSERT_TRUE(toks[0].type == TokenType::STRING_LIT);
    ASSERT_EQ(toks[0].value, std::string("hello world"));
}

TEST(unterminated_string_throws_lexer_error) {
    ASSERT_THROW(lex("'hello"), LexerError);
}

TEST(unterminated_string_error_has_line_col) {
    try {
        lex("'hello");
        ASSERT_TRUE(false); // should not reach here
    } catch (const LexerError& e) {
        ASSERT_EQ(e.line, 1);
    }
}

// -----------------------------------------------------------------------
// Operators
// -----------------------------------------------------------------------
TEST(operator_eq) {
    ASSERT_TRUE(firstType("=") == TokenType::EQ);
}
TEST(operator_neq) {
    ASSERT_TRUE(firstType("!=") == TokenType::NEQ);
}
TEST(operator_lt) {
    ASSERT_TRUE(firstType("<") == TokenType::LT);
}
TEST(operator_gt) {
    ASSERT_TRUE(firstType(">") == TokenType::GT);
}
TEST(operator_lte) {
    ASSERT_TRUE(firstType("<=") == TokenType::LTE);
}
TEST(operator_gte) {
    ASSERT_TRUE(firstType(">=") == TokenType::GTE);
}
TEST(operator_plus) {
    ASSERT_TRUE(firstType("+") == TokenType::PLUS_OP);
}
TEST(operator_minus) {
    ASSERT_TRUE(firstType("-") == TokenType::MINUS);
}
TEST(operator_star) {
    ASSERT_TRUE(firstType("*") == TokenType::STAR);
}
TEST(operator_slash) {
    ASSERT_TRUE(firstType("/") == TokenType::SLASH);
}
TEST(operator_concat) {
    ASSERT_TRUE(firstType("||") == TokenType::CONCAT);
}

// -----------------------------------------------------------------------
// Punctuation
// -----------------------------------------------------------------------
TEST(punct_lparen) {
    ASSERT_TRUE(firstType("(") == TokenType::LPAREN);
}
TEST(punct_rparen) {
    ASSERT_TRUE(firstType(")") == TokenType::RPAREN);
}
TEST(punct_comma) {
    ASSERT_TRUE(firstType(",") == TokenType::COMMA);
}
TEST(punct_semicolon) {
    ASSERT_TRUE(firstType(";") == TokenType::SEMICOLON);
}
TEST(punct_dot) {
    ASSERT_TRUE(firstType(".") == TokenType::DOT);
}

// -----------------------------------------------------------------------
// Comments
// -----------------------------------------------------------------------
TEST(line_comment_skipped) {
    auto toks = lex("-- this is a comment\nslay");
    ASSERT_TRUE(toks[0].type == TokenType::SLAY);
}

TEST(line_comment_does_not_consume_newline_token) {
    // After the comment there should be slay then EOF, not a newline token
    auto toks = lex("-- comment\nslay");
    ASSERT_EQ(toks.size(), (size_t)2);
    ASSERT_TRUE(toks[0].type == TokenType::SLAY);
    ASSERT_TRUE(toks[1].type == TokenType::EOF_TOKEN);
}

TEST(block_comment_skipped) {
    auto toks = lex("/* comment */ slay");
    ASSERT_TRUE(toks[0].type == TokenType::SLAY);
}

TEST(block_comment_multiline) {
    auto toks = lex("/*\n  multi\n  line\n*/slay");
    ASSERT_TRUE(toks[0].type == TokenType::SLAY);
}

TEST(inline_block_comment) {
    auto toks = lex("slay /* comment */ *");
    ASSERT_TRUE(toks[0].type == TokenType::SLAY);
    ASSERT_TRUE(toks[1].type == TokenType::STAR);
}

TEST(unterminated_block_comment_throws) {
    ASSERT_THROW(lex("/* unterminated"), LexerError);
}

TEST(unterminated_block_comment_error_has_position) {
    try {
        lex("/* unterminated");
        ASSERT_TRUE(false);
    } catch (const LexerError& e) {
        ASSERT_EQ(e.line, 1);
        ASSERT_EQ(e.col, 1);
    }
}

// -----------------------------------------------------------------------
// Error cases
// -----------------------------------------------------------------------
TEST(unexpected_char_at_sign_throws) {
    ASSERT_THROW(lex("@"), LexerError);
}

TEST(unexpected_char_hash_throws) {
    ASSERT_THROW(lex("#"), LexerError);
}

TEST(single_bang_throws) {
    ASSERT_THROW(lex("!"), LexerError);
}

TEST(single_pipe_throws) {
    ASSERT_THROW(lex("|"), LexerError);
}

// -----------------------------------------------------------------------
// Line / col tracking
// -----------------------------------------------------------------------
TEST(line_col_first_token) {
    auto toks = lex("slay");
    ASSERT_EQ(toks[0].line, 1);
    ASSERT_EQ(toks[0].col, 1);
}

TEST(line_increments_on_newline) {
    auto toks = lex("slay\nno-cap");
    ASSERT_EQ(toks[0].line, 1);
    ASSERT_EQ(toks[1].line, 2);
    ASSERT_EQ(toks[1].col, 1);
}

TEST(col_tracks_within_line) {
    auto toks = lex("slay *");
    // "slay" at col 1, " " skipped, "*" at col 6
    ASSERT_EQ(toks[0].col, 1);
    ASSERT_EQ(toks[1].col, 6);
}

TEST(lexer_error_reports_line_col) {
    try {
        lex("slay\n@");
        ASSERT_TRUE(false);
    } catch (const LexerError& e) {
        ASSERT_EQ(e.line, 2);
        ASSERT_EQ(e.col, 1);
    }
}

// -----------------------------------------------------------------------
// Multi-token sequences
// -----------------------------------------------------------------------
TEST(simple_select_star_from) {
    // "slay * no-cap users" -> SLAY STAR NO_CAP IDENTIFIER EOF
    auto toks = lex("slay * no-cap users");
    ASSERT_EQ(toks.size(), (size_t)5);
    ASSERT_TRUE(toks[0].type == TokenType::SLAY);
    ASSERT_TRUE(toks[1].type == TokenType::STAR);
    ASSERT_TRUE(toks[2].type == TokenType::NO_CAP);
    ASSERT_TRUE(toks[3].type == TokenType::IDENTIFIER);
    ASSERT_EQ(toks[3].value, std::string("users"));
    ASSERT_TRUE(toks[4].type == TokenType::EOF_TOKEN);
}

TEST(where_clause_tokens) {
    auto toks = lex("only-if id = 42");
    ASSERT_TRUE(toks[0].type == TokenType::ONLY_IF);
    ASSERT_TRUE(toks[1].type == TokenType::IDENTIFIER);
    ASSERT_TRUE(toks[2].type == TokenType::EQ);
    ASSERT_TRUE(toks[3].type == TokenType::INTEGER_LIT);
}

TEST(insert_tokens) {
    auto toks = lex("yeet-into users drip (1, 'Alice')");
    ASSERT_TRUE(toks[0].type == TokenType::YEET_INTO);
    ASSERT_TRUE(toks[1].type == TokenType::IDENTIFIER);
    ASSERT_TRUE(toks[2].type == TokenType::DRIP);
    ASSERT_TRUE(toks[3].type == TokenType::LPAREN);
    ASSERT_TRUE(toks[4].type == TokenType::INTEGER_LIT);
    ASSERT_TRUE(toks[5].type == TokenType::COMMA);
    ASSERT_TRUE(toks[6].type == TokenType::STRING_LIT);
    ASSERT_TRUE(toks[7].type == TokenType::RPAREN);
}

TEST(qualified_column_ref_tokens) {
    auto toks = lex("u.id");
    ASSERT_TRUE(toks[0].type == TokenType::IDENTIFIER);
    ASSERT_EQ(toks[0].value, std::string("u"));
    ASSERT_TRUE(toks[1].type == TokenType::DOT);
    ASSERT_TRUE(toks[2].type == TokenType::IDENTIFIER);
    ASSERT_EQ(toks[2].value, std::string("id"));
}

TEST(multiple_statements_separated_by_semicolon) {
    auto toks = lex("slay * no-cap a ; slay * no-cap b");
    // Tokens: SLAY STAR NO_CAP IDENT SEMI SLAY STAR NO_CAP IDENT EOF
    bool foundSemi = false;
    for (auto& t : toks) {
        if (t.type == TokenType::SEMICOLON) { foundSemi = true; break; }
    }
    ASSERT_TRUE(foundSemi);
}

TEST(headcount_star_tokens) {
    auto toks = lex("headcount(*)");
    ASSERT_TRUE(toks[0].type == TokenType::HEADCOUNT);
    ASSERT_TRUE(toks[1].type == TokenType::LPAREN);
    ASSERT_TRUE(toks[2].type == TokenType::STAR);
    ASSERT_TRUE(toks[3].type == TokenType::RPAREN);
}

TEST(comparison_operators_sequence) {
    auto toks = lex("a != b");
    ASSERT_TRUE(toks[1].type == TokenType::NEQ);
    auto toks2 = lex("a <= b");
    ASSERT_TRUE(toks2[1].type == TokenType::LTE);
    auto toks3 = lex("a >= b");
    ASSERT_TRUE(toks3[1].type == TokenType::GTE);
}

int main() {
    return run_all_tests("Lexer");
}
