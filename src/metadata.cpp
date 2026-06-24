#include "metadata.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(ch >> 4) & 0x0f];
                    out += hex[ch & 0x0f];
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out;
}

class JsonReader {
public:
    explicit JsonReader(std::string text) : text_(std::move(text)) {}

    std::unordered_map<std::string, TableMeta> parseCatalog() {
        std::unordered_map<std::string, TableMeta> result;
        expect('{');
        if (consume('}')) return result;

        do {
            const std::string key = parseString();
            expect(':');
            if (key == "tables") {
                result = parseTables();
            } else {
                skipValue();
            }
        } while (consume(','));

        expect('}');
        finish();
        return result;
    }

private:
    std::string text_;
    std::size_t pos_ = 0;

    void whitespace() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char expected) {
        whitespace();
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            throw std::runtime_error("Malformed catalog JSON");
        }
    }

    void finish() {
        whitespace();
        if (pos_ != text_.size()) {
            throw std::runtime_error("Trailing catalog JSON content");
        }
    }

    std::string parseString() {
        whitespace();
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            throw std::runtime_error("Expected JSON string");
        }
        ++pos_;

        std::string out;
        while (pos_ < text_.size()) {
            char ch = text_[pos_++];
            if (ch == '"') return out;
            if (ch != '\\') {
                out += ch;
                continue;
            }
            if (pos_ >= text_.size()) {
                throw std::runtime_error("Invalid JSON escape");
            }
            const char escaped = text_[pos_++];
            switch (escaped) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) {
                        throw std::runtime_error("Invalid Unicode escape");
                    }
                    unsigned value = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char hex = text_[pos_++];
                        value <<= 4;
                        if (hex >= '0' && hex <= '9') value += hex - '0';
                        else if (hex >= 'a' && hex <= 'f') value += hex - 'a' + 10;
                        else if (hex >= 'A' && hex <= 'F') value += hex - 'A' + 10;
                        else throw std::runtime_error("Invalid Unicode escape");
                    }
                    if (value <= 0x7f) {
                        out += static_cast<char>(value);
                    } else {
                        throw std::runtime_error("Non-ASCII catalog escape is unsupported");
                    }
                    break;
                }
                default:
                    throw std::runtime_error("Invalid JSON escape");
            }
        }
        throw std::runtime_error("Unterminated JSON string");
    }

    bool parseBool() {
        whitespace();
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return false;
        }
        throw std::runtime_error("Expected JSON boolean");
    }

    std::unordered_map<std::string, TableMeta> parseTables() {
        std::unordered_map<std::string, TableMeta> result;
        expect('{');
        if (consume('}')) return result;

        do {
            const std::string name = parseString();
            expect(':');
            TableMeta table = parseTable(name);
            result[name] = std::move(table);
        } while (consume(','));

        expect('}');
        return result;
    }

    TableMeta parseTable(const std::string& name) {
        TableMeta table;
        table.name = name;
        expect('{');
        if (consume('}')) return table;

        do {
            const std::string key = parseString();
            expect(':');
            if (key == "columns") {
                table.columns = parseColumns();
            } else {
                skipValue();
            }
        } while (consume(','));

        expect('}');
        return table;
    }

    std::vector<ColumnMeta> parseColumns() {
        std::vector<ColumnMeta> columns;
        expect('[');
        if (consume(']')) return columns;

        do {
            columns.push_back(parseColumn());
        } while (consume(','));

        expect(']');
        return columns;
    }

    ColumnMeta parseColumn() {
        ColumnMeta column;
        expect('{');
        if (consume('}')) return column;

        do {
            const std::string key = parseString();
            expect(':');
            if (key == "name") column.name = parseString();
            else if (key == "type") column.type = parseString();
            else if (key == "primary_key") column.primary_key = parseBool();
            else if (key == "not_null") column.not_null = parseBool();
            else if (key == "fk_table") column.fk_table = parseString();
            else if (key == "fk_col") column.fk_col = parseString();
            else skipValue();
        } while (consume(','));

        expect('}');
        if (column.type.empty()) column.type = "TEXT";
        return column;
    }

    void skipValue() {
        whitespace();
        if (pos_ >= text_.size()) {
            throw std::runtime_error("Missing JSON value");
        }

        const char ch = text_[pos_];
        if (ch == '"') {
            (void)parseString();
            return;
        }
        if (ch == '{') {
            ++pos_;
            if (consume('}')) return;
            do {
                (void)parseString();
                expect(':');
                skipValue();
            } while (consume(','));
            expect('}');
            return;
        }
        if (ch == '[') {
            ++pos_;
            if (consume(']')) return;
            do {
                skipValue();
            } while (consume(','));
            expect(']');
            return;
        }

        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const char current = text_[pos_];
            if (std::isspace(static_cast<unsigned char>(current)) ||
                current == ',' || current == '}' || current == ']') {
                break;
            }
            ++pos_;
        }
        if (start == pos_) {
            throw std::runtime_error("Invalid JSON value");
        }
    }
};

} // namespace

void Catalog::load() {
    std::ifstream file(filePath_);
    if (!file.is_open()) return;

    try {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        auto loaded = JsonReader(buffer.str()).parseCatalog();
        for (auto& entry : loaded) {
            tables[entry.first] = std::move(entry.second);
        }
        ++revision_;
        recomputeFingerprint();
    } catch (const std::exception&) {
        // A malformed catalog should not prevent the compiler from starting.
    }
}

void Catalog::save() {
    std::ofstream file(filePath_);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot write catalog file: " +
                                 filePath_);
    }

    auto names = tableNames();
    std::sort(names.begin(), names.end());

    file << "{\n  \"tables\": {";
    for (std::size_t tableIndex = 0; tableIndex < names.size(); ++tableIndex) {
        const auto& name = names[tableIndex];
        const auto& table = tables.at(name);
        file << (tableIndex == 0 ? "\n" : ",\n")
             << "    \"" << escapeJson(name) << "\": {\n"
             << "      \"columns\": [";

        for (std::size_t columnIndex = 0;
             columnIndex < table.columns.size();
             ++columnIndex) {
            const auto& column = table.columns[columnIndex];
            file << (columnIndex == 0 ? "\n" : ",\n")
                 << "        {"
                 << "\"name\": \"" << escapeJson(column.name) << "\", "
                 << "\"type\": \"" << escapeJson(column.type) << "\", "
                 << "\"primary_key\": "
                 << (column.primary_key ? "true" : "false") << ", "
                 << "\"not_null\": "
                 << (column.not_null ? "true" : "false") << ", "
                 << "\"fk_table\": \"" << escapeJson(column.fk_table) << "\", "
                 << "\"fk_col\": \"" << escapeJson(column.fk_col) << "\""
                 << "}";
        }
        if (!table.columns.empty()) file << "\n      ";
        file << "]\n    }";
    }
    if (!names.empty()) file << "\n  ";
    file << "}\n}\n";
}

void Catalog::addTable(const TableMeta& table) {
    tables[table.name] = table;
    ++revision_;
    recomputeFingerprint();
}

void Catalog::removeTable(const std::string& name) {
    if (tables.erase(name) > 0) {
        ++revision_;
        recomputeFingerprint();
    }
}

bool Catalog::hasTable(const std::string& name) const {
    return tables.count(name) > 0;
}

const TableMeta* Catalog::getTable(const std::string& name) const {
    auto it = tables.find(name);
    if (it == tables.end()) return nullptr;
    return &it->second;
}

std::string Catalog::validateColumnRef(const std::string& table,
                                       const std::string& col) const {
    if (table.empty()) return "";

    const auto* metadata = getTable(table);
    if (!metadata) return "Unknown table: " + table;

    for (const auto& column : metadata->columns) {
        if (column.name == col) return "";
    }
    return "Column '" + col + "' not found in table '" + table + "'";
}

std::vector<std::string> Catalog::tableNames() const {
    std::vector<std::string> names;
    names.reserve(tables.size());
    for (const auto& entry : tables) {
        names.push_back(entry.first);
    }
    return names;
}

void Catalog::recomputeFingerprint() {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;

    auto append = [&](const std::string& value) {
        for (unsigned char ch : value) {
            hash ^= ch;
            hash *= prime;
        }
        hash ^= 0xff;
        hash *= prime;
    };

    auto names = tableNames();
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        append(name);
        const auto& table = tables.at(name);
        for (const auto& column : table.columns) {
            append(column.name);
            append(column.type);
            append(column.primary_key ? "1" : "0");
            append(column.not_null ? "1" : "0");
            append(column.fk_table);
            append(column.fk_col);
        }
    }
    schemaFingerprint_ = hash;
}
