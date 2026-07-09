#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

struct ColumnMeta {
    std::string name;
    std::string type;       // INTEGER, TEXT, REAL, BLOB
    bool primary_key = false;
    bool not_null = false;
    std::string fk_table;   // foreign key table (empty if none)
    std::string fk_col;     // foreign key column
};

struct TableMeta {
    std::string name;
    std::vector<ColumnMeta> columns;
};

struct SnapshotFeatureMeta {
    std::string name;
    std::string spec;
};

struct SnapshotRowMeta {
    std::string rowid;  // stable page:slot row id in the source heap
    std::string split;  // train / validation / test
};

struct DatasetSnapshotMeta {
    std::string name;
    std::string queryText;
    std::string sourceTable;
    std::string schemaFingerprint;
    std::string tableVersion;
    std::string splitBy;
    std::uint64_t seed = 0;
    std::vector<SnapshotFeatureMeta> features;
    SnapshotFeatureMeta label;
    std::vector<SnapshotRowMeta> rows;
    std::vector<std::string> warnings;
};

struct ContextMessageMeta {
    std::uint64_t id = 0;
    std::string speaker;
    std::string text;
    std::string tab;
    std::string schemaName = "ConversationMessage";
    std::string schemaVersion = "v1";
    std::string storageRoute =
        "structured=catalog.contexts.messages; vector=ConversationMessage.content; blob=none";
    std::vector<std::string> accessLabels;
    std::vector<std::string> mentionedEntities;
};

struct ContextAtomMeta {
    std::string key;
    std::string value;
    std::string type;
    std::string status;        // active / invalidated
    std::string source;        // message_<id>
    std::string invalidatedBy; // message_<id>
    std::string tab;
    std::string schemaName = "ContextAtom";
    std::string schemaVersion = "v1";
    std::vector<std::string> accessLabels;
};

struct ContextTabAliasMeta {
    std::string alias;
    std::string target;
};

struct ContextSchemaMeta {
    std::string name;
    std::string version;
    std::string ownerAgentId;
    std::string sensitivityLevel;
    std::string retentionPolicy;
    std::string storageBackend;
    std::string vectorizationStrategy;
    std::vector<std::string> vectorizedFields;
    std::vector<std::string> accessLabels;
    std::vector<std::string> indexedFields;
    std::vector<std::string> relatedSchemas;
};

struct ConversationContextMeta {
    std::string name;
    std::vector<ContextMessageMeta> messages;
    std::vector<ContextAtomMeta> atoms;
    std::vector<ContextTabAliasMeta> tabAliases;
};

class Catalog {
public:
    static constexpr const char* CATALOG_FILE = ".skibidi_catalog.json";

    explicit Catalog(std::string filePath = CATALOG_FILE)
        : filePath_(std::move(filePath)) {}

    void load();
    void save();

    void addTable(const TableMeta& table);
    void removeTable(const std::string& name);
    bool hasTable(const std::string& name) const;
    const TableMeta* getTable(const std::string& name) const;

    void addSnapshot(const DatasetSnapshotMeta& snapshot);
    void removeSnapshot(const std::string& name);
    bool hasSnapshot(const std::string& name) const;
    const DatasetSnapshotMeta* getSnapshot(const std::string& name) const;

    void addContext(const ConversationContextMeta& context);
    void removeContext(const std::string& name);
    bool hasContext(const std::string& name) const;
    const ConversationContextMeta* getContext(const std::string& name) const;

    // Returns empty string on success, error message on failure
    std::string validateColumnRef(const std::string& table, const std::string& col) const;

    std::vector<std::string> tableNames() const;
    std::vector<std::string> snapshotNames() const;
    std::vector<std::string> contextNames() const;
    std::vector<ContextSchemaMeta> contextSchemas() const;
    std::uint64_t revision() const { return revision_; }
    std::uint64_t schemaFingerprint() const { return schemaFingerprint_; }

private:
    std::unordered_map<std::string, TableMeta> tables;
    std::unordered_map<std::string, DatasetSnapshotMeta> snapshots;
    std::unordered_map<std::string, ConversationContextMeta> contexts;
    std::string filePath_;
    std::uint64_t revision_ = 0;
    std::uint64_t schemaFingerprint_ = 1469598103934665603ULL;

    void recomputeFingerprint();
};
