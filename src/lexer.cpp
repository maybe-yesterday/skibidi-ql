#include "lexer.h"
#include <cctype>
#include <sstream>

// Keyword map - hyphenated tokens match as a single unit
// Longest-match is handled by reading the full hyphenated word before lookup
const std::unordered_map<std::string, TokenType> Lexer::KEYWORDS = {
    // Multi-word hyphenated keywords (longest first by convention, but we do full-word lookup)
    {"no-cap-not",      TokenType::NO_CAP_NOT},
    {"left-link-up",    TokenType::LEFT_LINK_UP},
    {"mid-link-up",     TokenType::MID_LINK_UP},
    {"link-up",         TokenType::LINK_UP},
    {"no-cap",          TokenType::NO_CAP},
    {"only-if",         TokenType::ONLY_IF},
    {"fr-fr",           TokenType::FR_FR},
    {"vibe-check",      TokenType::VIBE_CHECK},
    {"hits-different",  TokenType::HITS_DIFFERENT},
    {"bussin-only",     TokenType::BUSSIN_ONLY},
    {"yeet-into",       TokenType::YEET_INTO},
    {"glow-up",         TokenType::GLOW_UP},
    {"be-like",         TokenType::BE_LIKE},
    {"rizz-down",       TokenType::RIZZ_DOWN},
    {"main-character",  TokenType::MAIN_CHARACTER},
    {"side-character",  TokenType::SIDE_CHARACTER},
    {"unique-fr",       TokenType::UNIQUE_FR},
    {"cap-at",          TokenType::CAP_AT},
    {"up-only",         TokenType::UP_ONLY},
    {"down-bad",        TokenType::DOWN_BAD},
    {"or-nah",          TokenType::OR_NAH},
    {"biggest-W",       TokenType::BIGGEST_W},
    {"biggest-L",       TokenType::BIGGEST_L},
    {"mid-fr",          TokenType::MID_FR},
    {"percent-check",   TokenType::PERCENT_CHECK},
    {"split-by",        TokenType::SPLIT_BY},
    // Single-word keywords
    {"slay",            TokenType::SLAY},
    {"lowkey",          TokenType::LOWKEY},
    {"ghosted",         TokenType::GHOSTED},
    {"drip",            TokenType::DRIP},
    {"ratio",           TokenType::RATIO},
    {"manifest",        TokenType::MANIFEST},
    {"headcount",       TokenType::HEADCOUNT},
    {"stack",           TokenType::STACK},
    {"mid",             TokenType::MID},
    {"goat",            TokenType::GOAT},
    {"era",             TokenType::ERA},
    {"skip",            TokenType::SKIP},
    {"plus",            TokenType::PLUS},
    // Type names treated as identifiers by parser, but recognized
    // Special: L is MIN function - handled in readWord
};

Lexer::Lexer(std::string source)
    : src(std::move(source)), pos(0), line(1), col(1) {}

char Lexer::peek(size_t offset) const {
    size_t idx = pos + offset;
    if (idx >= src.size()) return '\0';
    return src[idx];
}

char Lexer::advance() {
    char c = src[pos++];
    if (c == '\n') {
        line++;
        col = 1;
    } else {
        col++;
    }
    return c;
}

void Lexer::skipWhitespace() {
    while (pos < src.size() && std::isspace((unsigned char)peek())) {
        advance();
    }
}

void Lexer::skipLineComment() {
    // consume until newline
    while (pos < src.size() && peek() != '\n') {
        advance();
    }
}

void Lexer::skipBlockComment() {
    int startLine = line, startCol = col;
    advance(); advance(); // consume /*
    while (pos < src.size()) {
        if (peek() == '*' && peek(1) == '/') {
            advance(); advance();
            return;
        }
        advance();
    }
    throw LexerError("Unterminated block comment", startLine, startCol);
}

bool Lexer::isWordStart(char c) const {
    return std::isalpha((unsigned char)c) || c == '_';
}

bool Lexer::isWordContinue(char c) const {
    // Identifiers and keywords can contain letters, digits, underscores, and hyphens
    // But hyphen is only valid in keyword tokens - we read greedily then look up
    return std::isalnum((unsigned char)c) || c == '_' || c == '-';
}

Token Lexer::readWord() {
    int startLine = line, startCol = col;
    std::string word;

    // Read characters that could be part of a keyword or identifier
    // We need to be careful: a hyphen could be a minus operator if not part of a keyword
    // Strategy: read greedily, then trim trailing hyphens if lookup fails
    while (pos < src.size() && isWordContinue(peek())) {
        // Don't consume a hyphen that's followed by a non-word char (it's a minus)
        if (peek() == '-') {
            // Look ahead: is the next char a letter or underscore?
            if (pos + 1 < src.size() && isWordStart(peek(1))) {
                word += advance();
            } else {
                break;
            }
        } else {
            word += advance();
        }
    }

    // Special case: single "L" is the MIN function
    if (word == "L") {
        return Token(TokenType::L_FUNC, word, startLine, startCol);
    }

    // Look up in keyword table
    auto it = KEYWORDS.find(word);
    if (it != KEYWORDS.end()) {
        return Token(it->second, word, startLine, startCol);
    }

    // Not a keyword - it's an identifier
    // But identifiers shouldn't contain hyphens in SQL contexts
    // If we read something like "foo-bar" and it's not a keyword, treat "foo" as identifier
    // and leave "-bar" for the next token
    // Find the first hyphen
    size_t hyphenPos = word.find('-');
    if (hyphenPos != std::string::npos) {
        // Back up: re-emit the portion before the first hyphen as identifier
        // and put the rest back
        std::string ident = word.substr(0, hyphenPos);
        std::string rest = word.substr(hyphenPos);
        // Put rest back into source by adjusting position
        // We need to move pos back by rest.size()
        pos -= rest.size();
        // Also fix line/col (all rest chars are on same line since no newline in word)
        col -= (int)rest.size();
        return Token(TokenType::IDENTIFIER, ident, startLine, startCol);
    }

    return Token(TokenType::IDENTIFIER, word, startLine, startCol);
}

Token Lexer::readNumber() {
    int startLine = line, startCol = col;
    std::string num;
    bool isFloat = false;

    while (pos < src.size() && std::isdigit((unsigned char)peek())) {
        num += advance();
    }

    if (pos < src.size() && peek() == '.' && pos + 1 < src.size() && std::isdigit((unsigned char)peek(1))) {
        isFloat = true;
        num += advance(); // consume '.'
        while (pos < src.size() && std::isdigit((unsigned char)peek())) {
            num += advance();
        }
    }

    // Optional exponent
    if (pos < src.size() && (peek() == 'e' || peek() == 'E')) {
        isFloat = true;
        num += advance();
        if (pos < src.size() && (peek() == '+' || peek() == '-')) {
            num += advance();
        }
        if (pos >= src.size() || !std::isdigit((unsigned char)peek())) {
            throw LexerError("Invalid number literal", startLine, startCol);
        }
        while (pos < src.size() && std::isdigit((unsigned char)peek())) {
            num += advance();
        }
    }

    return Token(isFloat ? TokenType::FLOAT_LIT : TokenType::INTEGER_LIT, num, startLine, startCol);
}

Token Lexer::readString() {
    int startLine = line, startCol = col;
    advance(); // consume opening '
    std::string value;

    while (pos < src.size()) {
        char c = peek();
        if (c == '\'') {
            // Check for escaped quote ('')
            if (pos + 1 < src.size() && peek(1) == '\'') {
                advance(); advance();
                value += '\'';
            } else {
                advance(); // consume closing '
                return Token(TokenType::STRING_LIT, value, startLine, startCol);
            }
        } else if (c == '\\') {
            advance(); // consume backslash
            if (pos >= src.size()) break;
            char esc = advance();
            switch (esc) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '\\': value += '\\'; break;
                case '\'': value += '\''; break;
                default: value += '\\'; value += esc; break;
            }
        } else {
            value += advance();
        }
    }

    throw LexerError("Unterminated string literal", startLine, startCol);
}

Token Lexer::readOperator() {
    int startLine = line, startCol = col;
    char c = peek();

    switch (c) {
        case '=':
            advance();
            return Token(TokenType::EQ, "=", startLine, startCol);
        case '!':
            if (peek(1) == '=') {
                advance(); advance();
                return Token(TokenType::NEQ, "!=", startLine, startCol);
            }
            throw LexerError(std::string("Unexpected character '") + c + "'", startLine, startCol);
        case '<':
            advance();
            if (pos < src.size() && peek() == '=') {
                advance();
                return Token(TokenType::LTE, "<=", startLine, startCol);
            }
            return Token(TokenType::LT, "<", startLine, startCol);
        case '>':
            advance();
            if (pos < src.size() && peek() == '=') {
                advance();
                return Token(TokenType::GTE, ">=", startLine, startCol);
            }
            return Token(TokenType::GT, ">", startLine, startCol);
        case '+':
            advance();
            return Token(TokenType::PLUS_OP, "+", startLine, startCol);
        case '-':
            advance();
            return Token(TokenType::MINUS, "-", startLine, startCol);
        case '*':
            advance();
            return Token(TokenType::STAR, "*", startLine, startCol);
        case '/':
            advance();
            return Token(TokenType::SLASH, "/", startLine, startCol);
        case '|':
            if (peek(1) == '|') {
                advance(); advance();
                return Token(TokenType::CONCAT, "||", startLine, startCol);
            }
            throw LexerError("Expected '||' for concatenation", startLine, startCol);
        case '(':
            advance();
            return Token(TokenType::LPAREN, "(", startLine, startCol);
        case ')':
            advance();
            return Token(TokenType::RPAREN, ")", startLine, startCol);
        case ',':
            advance();
            return Token(TokenType::COMMA, ",", startLine, startCol);
        case ';':
            advance();
            return Token(TokenType::SEMICOLON, ";", startLine, startCol);
        case '.':
            advance();
            return Token(TokenType::DOT, ".", startLine, startCol);
        default:
            throw LexerError(std::string("Unexpected character '") + c + "'", startLine, startCol);
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (true) {
        skipWhitespace();

        if (pos >= src.size()) {
            tokens.emplace_back(TokenType::EOF_TOKEN, "", line, col);
            break;
        }

        char c = peek();

        // Line comment
        if (c == '-' && peek(1) == '-') {
            skipLineComment();
            continue;
        }

        // Block comment
        if (c == '/' && peek(1) == '*') {
            skipBlockComment();
            continue;
        }

        // String literal
        if (c == '\'') {
            tokens.push_back(readString());
            continue;
        }

        // Number
        if (std::isdigit((unsigned char)c)) {
            tokens.push_back(readNumber());
            continue;
        }

        // Word (keyword or identifier)
        if (isWordStart(c)) {
            tokens.push_back(readWord());
            continue;
        }

        // Operators and punctuation
        tokens.push_back(readOperator());
    }

    return tokens;
}
