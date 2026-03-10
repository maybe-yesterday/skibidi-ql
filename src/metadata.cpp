#include "metadata.h"
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

void Catalog::load() {
    std::ifstream f(CATALOG_FILE);
    if (!f.is_open()) return; // no catalog file yet

    try {
        json j;
        f >> j;

        if (!j.contains("tables")) return;

        for (auto& [tname, tval] : j["tables"].items()) {
            TableMeta tm;
            tm.name = tname;

            if (tval.contains("columns")) {
                for (auto& col : tval["columns"]) {
                    ColumnMeta cm;
                    cm.name = col.value("name", "");
                    cm.type = col.value("type", "TEXT");
                    cm.primary_key = col.value("primary_key", false);
                    cm.not_null = col.value("not_null", false);
                    cm.fk_table = col.value("fk_table", "");
                    cm.fk_col = col.value("fk_col", "");
                    tm.columns.push_back(std::move(cm));
                }
            }

            tables[tname] = std::move(tm);
        }
    } catch (const std::exception& e) {
        // Catalog file exists but is malformed - ignore and start fresh
    }
}

void Catalog::save() {
    json j;
    j["tables"] = json::object();

    for (auto& [tname, tm] : tables) {
        json cols = json::array();
        for (auto& cm : tm.columns) {
            json c;
            c["name"] = cm.name;
            c["type"] = cm.type;
            c["primary_key"] = cm.primary_key;
            c["not_null"] = cm.not_null;
            c["fk_table"] = cm.fk_table;
            c["fk_col"] = cm.fk_col;
            cols.push_back(c);
        }
        j["tables"][tname]["columns"] = cols;
    }

    std::ofstream f(CATALOG_FILE);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot write catalog file: " + std::string(CATALOG_FILE));
    }
    f << j.dump(2) << "\n";
}

void Catalog::addTable(const TableMeta& table) {
    tables[table.name] = table;
}

void Catalog::removeTable(const std::string& name) {
    tables.erase(name);
}

bool Catalog::hasTable(const std::string& name) const {
    return tables.count(name) > 0;
}

const TableMeta* Catalog::getTable(const std::string& name) const {
    auto it = tables.find(name);
    if (it == tables.end()) return nullptr;
    return &it->second;
}

std::string Catalog::validateColumnRef(const std::string& table, const std::string& col) const {
    if (table.empty()) {
        // Can't validate without table context
        return "";
    }

    auto* tm = getTable(table);
    if (!tm) {
        return "Unknown table: " + table;
    }

    for (auto& cm : tm->columns) {
        if (cm.name == col) return "";
    }

    return "Column '" + col + "' not found in table '" + table + "'";
}

std::vector<std::string> Catalog::tableNames() const {
    std::vector<std::string> names;
    names.reserve(tables.size());
    for (auto& [name, _] : tables) {
        names.push_back(name);
    }
    return names;
}
