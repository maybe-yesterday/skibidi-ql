#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>

enum class TokenType {
    // Keywords - SELECT family
    SLAY,           // SELECT
    NO_CAP,         // FROM
    ONLY_IF,        // WHERE
    LINK_UP,        // JOIN
    LEFT_LINK_UP,   // LEFT JOIN
    MID_LINK_UP,    // INNER JOIN
    FR_FR,          // ON
    VIBE_CHECK,     // GROUP BY
    HITS_DIFFERENT, // ORDER BY
    BUSSIN_ONLY,    // HAVING
    LOWKEY,         // AS
    PLUS,           // AND
    OR_NAH,         // OR
    NO_CAP_NOT,     // NOT
    GHOSTED,        // NULL
    UNIQUE_FR,      // DISTINCT
    CAP_AT,         // LIMIT
    SKIP,           // OFFSET
    UP_ONLY,        // ASC
    DOWN_BAD,       // DESC

    // DML keywords
    YEET_INTO,      // INSERT INTO
    DRIP,           // VALUES
    GLOW_UP,        // UPDATE
    BE_LIKE,        // SET
    RATIO,          // DELETE FROM

    // DDL keywords
    MANIFEST,       // CREATE TABLE
    RIZZ_DOWN,      // DROP TABLE

    // Constraints
    MAIN_CHARACTER, // PRIMARY KEY
    SIDE_CHARACTER, // FOREIGN KEY

    // Aggregate functions
    HEADCOUNT,      // COUNT
    STACK,          // SUM
    MID,            // AVG
    GOAT,           // MAX
    L_FUNC,         // MIN (single letter L)

    // Advanced analytics
    BIGGEST_W,      // ARGMAX
    BIGGEST_L,      // ARGMIN
    MID_FR,         // MEDIAN
    PERCENT_CHECK,  // PERCENTILE
    ERA,            // RANK() OVER
    SPLIT_BY,       // PARTITION BY

    // Literals
    INTEGER_LIT,
    FLOAT_LIT,
    STRING_LIT,
    TRUE_LIT,
    FALSE_LIT,

    // Identifiers
    IDENTIFIER,

    // Operators
    EQ,         // =
    NEQ,        // !=
    LT,         // <
    GT,         // >
    LTE,        // <=
    GTE,        // >=
    PLUS_OP,    // + (arithmetic)
    MINUS,      // -
    STAR,       // *
    SLASH,      // /
    CONCAT,     // ||

    // Punctuation
    LPAREN,     // (
    RPAREN,     // )
    COMMA,      // ,
    SEMICOLON,  // ;
    DOT,        // .

    // Special
    EOF_TOKEN
};

struct Token {
    TokenType type;
    std::string value;
    int line;
    int col;

    Token(TokenType t, std::string v, int l, int c)
        : type(t), value(std::move(v)), line(l), col(c) {}
};

class LexerError : public std::runtime_error {
public:
    int line, col;
    LexerError(const std::string& msg, int l, int c)
        : std::runtime_error("Error at line " + std::to_string(l) + ", col " + std::to_string(c) + ": " + msg),
          line(l), col(c) {}
};

class Lexer {
public:
    explicit Lexer(std::string source);
    std::vector<Token> tokenize();

private:
    std::string src;
    size_t pos;
    int line;
    int col;

    static const std::unordered_map<std::string, TokenType> KEYWORDS;

    char peek(size_t offset = 0) const;
    char advance();
    void skipWhitespace();
    void skipLineComment();
    void skipBlockComment();
    Token readWord();
    Token readNumber();
    Token readString();
    Token readOperator();

    bool isWordStart(char c) const;
    bool isWordContinue(char c) const;
};
