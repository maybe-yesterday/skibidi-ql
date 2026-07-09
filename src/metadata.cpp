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

void writeJsonStringArray(std::ostream& out,
                          const std::vector<std::string>& values) {
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) out << ", ";
        out << "\"" << escapeJson(values[index]) << "\"";
    }
    out << "]";
}

struct CatalogData {
    std::unordered_map<std::string, TableMeta> tables;
    std::unordered_map<std::string, DatasetSnapshotMeta> snapshots;
    std::unordered_map<std::string, ConversationContextMeta> contexts;
};

class JsonReader {
public:
    explicit JsonReader(std::string text) : text_(std::move(text)) {}

    CatalogData parseCatalog() {
        CatalogData result;
        expect('{');
        if (consume('}')) return result;

        do {
            const std::string key = parseString();
            expect(':');
            if (key == "tables") {
                result.tables = parseTables();
            } else if (key == "snapshots") {
                result.snapshots = parseSnapshots();
            } else if (key == "contexts") {
                result.contexts = parseContexts();
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

    std::uint64_t parseUnsigned() {
        whitespace();
        const std::size_t start = pos_;
        while (pos_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        if (start == pos_) {
            throw std::runtime_error("Expected JSON integer");
        }
        return static_cast<std::uint64_t>(
            std::stoull(text_.substr(start, pos_ - start)));
    }

    std::vector<std::string> parseStringArray() {
        std::vector<std::string> result;
        expect('[');
        if (consume(']')) return result;

        do {
            result.push_back(parseString());
        } while (consume(','));

        expect(']');
        return result;
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

    std::unordered_map<std::string, DatasetSnapshotMeta> parseSnapshots() {
        std::unordered_map<std::string, DatasetSnapshotMeta> result;
        expect('{');
        if (consume('}')) return result;

        do {
            const std::string name = parseString();
            expect(':');
            DatasetSnapshotMeta snapshot = parseSnapshot(name);
            result[name] = std::move(snapshot);
        } while (consume(','));

        expect('}');
        return result;
    }

    DatasetSnapshotMeta parseSnapshot(const std::string& name) {
        DatasetSnapshotMeta snapshot;
        snapshot.name = name;
        expect('{');
        if (consume('}')) return snapshot;

        do {
            const std::string key = parseString();
            expect(':');
            if (key == "query_text") snapshot.queryText = parseString();
            else if (key == "source_table") snapshot.sourceTable = parseString();
            else if (key == "schema_fingerprint") snapshot.schemaFingerprint = parseString();
            else if (key == "table_version") snapshot.tableVersion = parseString();
            else if (key == "split_by") snapshot.splitBy = parseString();
            else if (key == "seed") snapshot.seed = parseUnsigned();
            else if (key == "features") snapshot.features = parseFeatures();
            else if (key == "label") snapshot.label = parseFeature();
            else if (key == "rows") snapshot.rows = parseRows();
            else if (key == "warnings") snapshot.warnings = parseStringArray();
            else skipValue();
        } while (consume(','));

        expect('}');
        return snapshot;
    }

    std::vector<SnapshotFeatureMeta> parseFeatures() {
        std::vector<SnapshotFeatureMeta> result;
        expect('[');
        if (consume(']')) return result;

        do {
            result.push_back(parseFeature());
        } while (consume(','));

        expect(']');
        return result;
    }

    SnapshotFeatureMeta parseFeature() {
        SnapshotFeatureMeta feature;
        expect('{');
        if (consume('}')) return feature;

        do {
            const std::string key = parseString();
            expect(':');
            if (key == "name") feature.name = parseString();
            else if (key == "spec") feature.spec = parseString();
            else skipValue();
        } while (consume(','));

        expect('}');
        return feature;
    }

    std::vector<SnapshotRowMeta> parseRows() {
        std::vector<SnapshotRowMeta> result;
        expect('[');
        if (consume(']')) return result;

        do {
            SnapshotRowMeta row;
            expect('{');
            if (!consume('}')) {
                do {
                    const std::string key = parseString();
                    expect(':');
                    if (key == "rowid") row.rowid = parseString();
                    else if (key == "split") row.split = parseString();
                    else skipValue();
                } while (consume(','));
                expect('}');
            }
            result.push_back(std::move(row));
        } while (consume(','));

        expect(']');
        return result;
    }

    std::unordered_map<std::string, ConversationContextMeta>
    parseContexts() {
        std::unordered_map<std::string, ConversationContextMeta> result;
        expect('{');
        if (consume('}')) return result;

        do {
            const std::string name = parseString();
            expect(':');
            ConversationContextMeta context = parseContext(name);
            result[name] = std::move(context);
        } while (consume(','));

        expect('}');
        return result;
    }

    ConversationContextMeta parseContext(const std::string& name) {
        ConversationContextMeta context;
        context.name = name;
        expect('{');
        if (consume('}')) return context;

        do {
            const std::string key = parseString();
            expect(':');
            if (key == "messages") context.messages = parseMessages();
            else if (key == "atoms") context.atoms = parseAtoms();
            else if (key == "tab_aliases") {
                context.tabAliases = parseTabAliases();
            }
            else skipValue();
        } while (consume(','));

        expect('}');
        return context;
    }

    std::vector<ContextMessageMeta> parseMessages() {
        std::vector<ContextMessageMeta> result;
        expect('[');
        if (consume(']')) return result;

        do {
            ContextMessageMeta message;
            expect('{');
            if (!consume('}')) {
                do {
                    const std::string key = parseString();
                    expect(':');
                    if (key == "id") message.id = parseUnsigned();
                    else if (key == "speaker") message.speaker = parseString();
                    else if (key == "text") message.text = parseString();
                    else if (key == "tab") message.tab = parseString();
                    else if (key == "schema_name") {
                        message.schemaName = parseString();
                    } else if (key == "schema_version") {
                        message.schemaVersion = parseString();
                    } else if (key == "storage_route") {
                        message.storageRoute = parseString();
                    } else if (key == "access_labels") {
                        message.accessLabels = parseStringArray();
                    } else if (key == "mentioned_entities") {
                        message.mentionedEntities = parseStringArray();
                    }
                    else skipValue();
                } while (consume(','));
                expect('}');
            }
            if (message.tab.empty()) message.tab = "main";
            if (message.schemaName.empty()) {
                message.schemaName = "ConversationMessage";
            }
            if (message.schemaVersion.empty()) message.schemaVersion = "v1";
            if (message.storageRoute.empty()) {
                message.storageRoute =
                    "structured=catalog.contexts.messages; vector=ConversationMessage.content; blob=none";
            }
            result.push_back(std::move(message));
        } while (consume(','));

        expect(']');
        return result;
    }

    std::vector<ContextAtomMeta> parseAtoms() {
        std::vector<ContextAtomMeta> result;
        expect('[');
        if (consume(']')) return result;

        do {
            ContextAtomMeta atom;
            expect('{');
            if (!consume('}')) {
                do {
                    const std::string key = parseString();
                    expect(':');
                    if (key == "key") atom.key = parseString();
                    else if (key == "value") atom.value = parseString();
                    else if (key == "type") atom.type = parseString();
                    else if (key == "status") atom.status = parseString();
                    else if (key == "source") atom.source = parseString();
                    else if (key == "invalidated_by") {
                        atom.invalidatedBy = parseString();
                    } else if (key == "tab") {
                        atom.tab = parseString();
                    } else if (key == "schema_name") {
                        atom.schemaName = parseString();
                    } else if (key == "schema_version") {
                        atom.schemaVersion = parseString();
                    } else if (key == "access_labels") {
                        atom.accessLabels = parseStringArray();
                    } else skipValue();
                } while (consume(','));
                expect('}');
            }
            if (atom.tab.empty()) atom.tab = "main";
            if (atom.schemaName.empty()) atom.schemaName = "ContextAtom";
            if (atom.schemaVersion.empty()) atom.schemaVersion = "v1";
            result.push_back(std::move(atom));
        } while (consume(','));

        expect(']');
        return result;
    }

    std::vector<ContextTabAliasMeta> parseTabAliases() {
        std::vector<ContextTabAliasMeta> result;
        expect('[');
        if (consume(']')) return result;

        do {
            ContextTabAliasMeta alias;
            expect('{');
            if (!consume('}')) {
                do {
                    const std::string key = parseString();
                    expect(':');
                    if (key == "alias") alias.alias = parseString();
                    else if (key == "target") alias.target = parseString();
                    else skipValue();
                } while (consume(','));
                expect('}');
            }
            if (!alias.alias.empty() && !alias.target.empty()) {
                result.push_back(std::move(alias));
            }
        } while (consume(','));

        expect(']');
        return result;
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

std::vector<ContextSchemaMeta> Catalog::contextSchemas() const {
    return {
        ContextSchemaMeta{
            "ConversationMessage",
            "v1",
            "system",
            "agent-internal",
            "conversation-lifetime",
            "structured+vector",
            "embed content for semantic filtering; keep metadata structured",
            {"content"},
            {"AGENT_INTERNAL"},
            {"conversation_id", "sender_id", "sender_type", "timestamp",
             "tab", "mentioned_entities", "access_labels",
             "schema_name", "storage_route"},
            {"ContextAtom", "TaskState", "ToolInvocationLog"},
        },
        ContextSchemaMeta{
            "ContextAtom",
            "v1",
            "system",
            "agent-internal",
            "conversation-lifetime",
            "structured",
            "not vectorized; derived from source message content",
            {},
            {"AGENT_INTERNAL"},
            {"key", "type", "status", "tab", "source",
             "invalidated_by", "access_labels", "schema_name"},
            {"ConversationMessage"},
        },
        ContextSchemaMeta{
            "TaskState",
            "v1",
            "system",
            "agent-internal",
            "task-lifetime",
            "structured",
            "description may be embedded when semantic task lookup lands",
            {"description"},
            {"AGENT_INTERNAL"},
            {"task_id", "status", "assigned_agent_id",
             "last_update_timestamp"},
            {"ConversationMessage", "ToolInvocationLog"},
        },
        ContextSchemaMeta{
            "ToolInvocationLog",
            "v1",
            "system",
            "agent-internal",
            "audit-lifetime",
            "structured+blob",
            "large tool payloads stay as blob refs; summaries can be embedded",
            {"summary"},
            {"AGENT_INTERNAL", "TOOL_TRACE"},
            {"tool_id", "conversation_id", "message_id", "status",
             "timestamp"},
            {"ConversationMessage", "TaskState"},
        },
        ContextSchemaMeta{
            "UserProfile",
            "v1",
            "system",
            "confidential",
            "user-lifetime",
            "structured",
            "selected preference text can be embedded after policy check",
            {"preferences"},
            {"CONFIDENTIAL_CUSTOMER_DATA"},
            {"user_id", "tenant_id", "access_labels"},
            {"ConversationMessage"},
        },
    };
}

void Catalog::load() {
    std::ifstream file(filePath_);
    if (!file.is_open()) return;

    try {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        auto loaded = JsonReader(buffer.str()).parseCatalog();
        for (auto& entry : loaded.tables) {
            tables[entry.first] = std::move(entry.second);
        }
        for (auto& entry : loaded.snapshots) {
            snapshots[entry.first] = std::move(entry.second);
        }
        for (auto& entry : loaded.contexts) {
            contexts[entry.first] = std::move(entry.second);
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
    file << "},\n  \"snapshots\": {";

    auto snapshotNamesValue = snapshotNames();
    std::sort(snapshotNamesValue.begin(), snapshotNamesValue.end());
    for (std::size_t snapshotIndex = 0;
         snapshotIndex < snapshotNamesValue.size();
         ++snapshotIndex) {
        const auto& name = snapshotNamesValue[snapshotIndex];
        const auto& snapshot = snapshots.at(name);
        file << (snapshotIndex == 0 ? "\n" : ",\n")
             << "    \"" << escapeJson(name) << "\": {\n"
             << "      \"query_text\": \""
             << escapeJson(snapshot.queryText) << "\",\n"
             << "      \"source_table\": \""
             << escapeJson(snapshot.sourceTable) << "\",\n"
             << "      \"schema_fingerprint\": \""
             << escapeJson(snapshot.schemaFingerprint) << "\",\n"
             << "      \"table_version\": \""
             << escapeJson(snapshot.tableVersion) << "\",\n"
             << "      \"split_by\": \""
             << escapeJson(snapshot.splitBy) << "\",\n"
             << "      \"seed\": " << snapshot.seed << ",\n"
             << "      \"features\": [";

        for (std::size_t featureIndex = 0;
             featureIndex < snapshot.features.size();
             ++featureIndex) {
            const auto& feature = snapshot.features[featureIndex];
            file << (featureIndex == 0 ? "\n" : ",\n")
                 << "        {\"name\": \"" << escapeJson(feature.name)
                 << "\", \"spec\": \"" << escapeJson(feature.spec)
                 << "\"}";
        }
        if (!snapshot.features.empty()) file << "\n      ";
        file << "],\n"
             << "      \"label\": {\"name\": \""
             << escapeJson(snapshot.label.name) << "\", \"spec\": \""
             << escapeJson(snapshot.label.spec) << "\"},\n"
             << "      \"rows\": [";

        for (std::size_t rowIndex = 0;
             rowIndex < snapshot.rows.size();
             ++rowIndex) {
            const auto& row = snapshot.rows[rowIndex];
            file << (rowIndex == 0 ? "\n" : ",\n")
                 << "        {\"rowid\": \"" << escapeJson(row.rowid)
                 << "\", \"split\": \"" << escapeJson(row.split)
                 << "\"}";
        }
        if (!snapshot.rows.empty()) file << "\n      ";
        file << "],\n"
             << "      \"warnings\": [";

        for (std::size_t warningIndex = 0;
             warningIndex < snapshot.warnings.size();
             ++warningIndex) {
            file << (warningIndex == 0 ? "" : ", ")
                 << "\"" << escapeJson(snapshot.warnings[warningIndex])
                 << "\"";
        }
        file << "]\n"
             << "    }";
    }
    if (!snapshotNamesValue.empty()) file << "\n  ";
    file << "},\n  \"contexts\": {";

    auto contextNamesValue = contextNames();
    std::sort(contextNamesValue.begin(), contextNamesValue.end());
    for (std::size_t contextIndex = 0;
         contextIndex < contextNamesValue.size();
         ++contextIndex) {
        const auto& name = contextNamesValue[contextIndex];
        const auto& context = contexts.at(name);
        file << (contextIndex == 0 ? "\n" : ",\n")
             << "    \"" << escapeJson(name) << "\": {\n"
             << "      \"messages\": [";

        for (std::size_t messageIndex = 0;
             messageIndex < context.messages.size();
             ++messageIndex) {
            const auto& message = context.messages[messageIndex];
            file << (messageIndex == 0 ? "\n" : ",\n")
                 << "        {\"id\": " << message.id
                 << ", \"speaker\": \"" << escapeJson(message.speaker)
                 << "\", \"text\": \"" << escapeJson(message.text)
                 << "\", \"tab\": \"" << escapeJson(message.tab)
                 << "\", \"schema_name\": \""
                 << escapeJson(message.schemaName)
                 << "\", \"schema_version\": \""
                 << escapeJson(message.schemaVersion)
                 << "\", \"storage_route\": \""
                 << escapeJson(message.storageRoute)
                 << "\", \"access_labels\": ";
            writeJsonStringArray(file, message.accessLabels);
            file << ", \"mentioned_entities\": ";
            writeJsonStringArray(file, message.mentionedEntities);
            file << "}";
        }
        if (!context.messages.empty()) file << "\n      ";
        file << "],\n"
             << "      \"atoms\": [";

        for (std::size_t atomIndex = 0;
             atomIndex < context.atoms.size();
             ++atomIndex) {
            const auto& atom = context.atoms[atomIndex];
            file << (atomIndex == 0 ? "\n" : ",\n")
                 << "        {\"key\": \"" << escapeJson(atom.key)
                 << "\", \"value\": \"" << escapeJson(atom.value)
                 << "\", \"type\": \"" << escapeJson(atom.type)
                 << "\", \"status\": \"" << escapeJson(atom.status)
                 << "\", \"source\": \"" << escapeJson(atom.source)
                 << "\", \"invalidated_by\": \""
                 << escapeJson(atom.invalidatedBy)
                 << "\", \"tab\": \"" << escapeJson(atom.tab)
                 << "\", \"schema_name\": \""
                 << escapeJson(atom.schemaName)
                 << "\", \"schema_version\": \""
                 << escapeJson(atom.schemaVersion)
                 << "\", \"access_labels\": ";
            writeJsonStringArray(file, atom.accessLabels);
            file << "}";
        }
        if (!context.atoms.empty()) file << "\n      ";
        file << "],\n"
             << "      \"tab_aliases\": [";

        for (std::size_t aliasIndex = 0;
             aliasIndex < context.tabAliases.size();
             ++aliasIndex) {
            const auto& alias = context.tabAliases[aliasIndex];
            file << (aliasIndex == 0 ? "\n" : ",\n")
                 << "        {\"alias\": \"" << escapeJson(alias.alias)
                 << "\", \"target\": \"" << escapeJson(alias.target)
                 << "\"}";
        }
        if (!context.tabAliases.empty()) file << "\n      ";
        file << "]\n"
             << "    }";
    }
    if (!contextNamesValue.empty()) file << "\n  ";
    file << "}\n}\n";
}

void Catalog::addTable(const TableMeta& table) {
    tables[table.name] = table;
    ++revision_;
    recomputeFingerprint();
}

void Catalog::removeTable(const std::string& name) {
    if (tables.erase(name) > 0) {
        for (auto it = snapshots.begin(); it != snapshots.end(); ) {
            if (it->second.sourceTable == name) {
                it = snapshots.erase(it);
            } else {
                ++it;
            }
        }
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

void Catalog::addSnapshot(const DatasetSnapshotMeta& snapshot) {
    snapshots[snapshot.name] = snapshot;
    ++revision_;
    recomputeFingerprint();
}

void Catalog::removeSnapshot(const std::string& name) {
    if (snapshots.erase(name) > 0) {
        ++revision_;
        recomputeFingerprint();
    }
}

bool Catalog::hasSnapshot(const std::string& name) const {
    return snapshots.count(name) > 0;
}

const DatasetSnapshotMeta* Catalog::getSnapshot(
    const std::string& name) const {
    auto it = snapshots.find(name);
    if (it == snapshots.end()) return nullptr;
    return &it->second;
}

void Catalog::addContext(const ConversationContextMeta& context) {
    contexts[context.name] = context;
    ++revision_;
    recomputeFingerprint();
}

void Catalog::removeContext(const std::string& name) {
    if (contexts.erase(name) > 0) {
        ++revision_;
        recomputeFingerprint();
    }
}

bool Catalog::hasContext(const std::string& name) const {
    return contexts.count(name) > 0;
}

const ConversationContextMeta* Catalog::getContext(
    const std::string& name) const {
    auto it = contexts.find(name);
    if (it == contexts.end()) return nullptr;
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

std::vector<std::string> Catalog::snapshotNames() const {
    std::vector<std::string> names;
    names.reserve(snapshots.size());
    for (const auto& entry : snapshots) {
        names.push_back(entry.first);
    }
    return names;
}

std::vector<std::string> Catalog::contextNames() const {
    std::vector<std::string> names;
    names.reserve(contexts.size());
    for (const auto& entry : contexts) {
        names.push_back(entry.first);
    }
    return names;
}

void Catalog::recomputeFingerprint() {
    std::uint64_t hash = skibidi::hash::kFnv1a64OffsetBasis;

    auto append = [&](const std::string& value) {
        hash = skibidi::hash::fnv1a64Append(hash, value);
        hash = skibidi::hash::fnv1a64AppendByte(
            hash, skibidi::hash::kFnv1a64FieldSeparator);
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
    auto snapshotsValue = snapshotNames();
    std::sort(snapshotsValue.begin(), snapshotsValue.end());
    for (const auto& name : snapshotsValue) {
        append(name);
        const auto& snapshot = snapshots.at(name);
        append(snapshot.queryText);
        append(snapshot.sourceTable);
        append(snapshot.schemaFingerprint);
        append(snapshot.tableVersion);
        append(snapshot.splitBy);
        append(std::to_string(snapshot.seed));
        for (const auto& feature : snapshot.features) {
            append(feature.name);
            append(feature.spec);
        }
        append(snapshot.label.name);
        append(snapshot.label.spec);
        append(std::to_string(snapshot.rows.size()));
    }
    auto contextsValue = contextNames();
    std::sort(contextsValue.begin(), contextsValue.end());
    for (const auto& name : contextsValue) {
        append(name);
        const auto& context = contexts.at(name);
        for (const auto& message : context.messages) {
            append(std::to_string(message.id));
            append(message.speaker);
            append(message.text);
            append(message.tab);
        }
        for (const auto& atom : context.atoms) {
            append(atom.key);
            append(atom.value);
            append(atom.type);
            append(atom.status);
            append(atom.source);
            append(atom.invalidatedBy);
            append(atom.tab);
        }
        for (const auto& alias : context.tabAliases) {
            append(alias.alias);
            append(alias.target);
        }
    }
    schemaFingerprint_ = hash;
}
