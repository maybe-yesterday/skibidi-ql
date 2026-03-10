#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "nlohmann/json.hpp"

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

class Catalog {
public:
    static constexpr const char* CATALOG_FILE = ".skibidi_catalog.json";

    void load();
    void save();

    void addTable(const TableMeta& table);
    void removeTable(const std::string& name);
    bool hasTable(const std::string& name) const;
    const TableMeta* getTable(const std::string& name) const;

    // Returns empty string on success, error message on failure
    std::string validateColumnRef(const std::string& table, const std::string& col) const;

    std::vector<std::string> tableNames() const;

private:
    std::unordered_map<std::string, TableMeta> tables;
};
