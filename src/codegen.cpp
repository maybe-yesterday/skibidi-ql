#include "codegen.h"
#include <sstream>
#include <stdexcept>
#include <algorithm>

// -----------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------
std::string CodeGen::quoteIdent(const std::string& s) {
    // If identifier contains special chars or is a keyword, quote it
    // For simplicity, pass through as-is (SQLite is liberal)
    return s;
}

std::string CodeGen::quoteString(const std::string& s) {
    std::string result = "'";
    for (char c : s) {
        if (c == '\'') result += "''";
        else result += c;
    }
    result += "'";
    return result;
}

std::string CodeGen::joinWith(const std::vector<std::string>& parts, const std::string& sep) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += sep;
        result += parts[i];
    }
    return result;
}

// -----------------------------------------------------------------------
// Main entry point
// -----------------------------------------------------------------------
std::string CodeGen::generate(const ASTNode* node) {
    if (!node) return "";

    if (auto* s = dynamic_cast<const SelectStmt*>(node)) return genSelect(s);
    if (auto* s = dynamic_cast<const InsertStmt*>(node)) return genInsert(s);
    if (auto* s = dynamic_cast<const UpdateStmt*>(node)) return genUpdate(s);
    if (auto* s = dynamic_cast<const DeleteStmt*>(node)) return genDelete(s);
    if (auto* s = dynamic_cast<const CreateStmt*>(node)) return genCreate(s);
    if (auto* s = dynamic_cast<const DropStmt*>(node))   return genDrop(s);

    throw std::runtime_error("CodeGen: Unknown top-level node type");
}

// -----------------------------------------------------------------------
// SELECT
// -----------------------------------------------------------------------
std::string CodeGen::genSelect(const SelectStmt* s) {
    // Check for special analytics functions that require CTE rewrites:
    // mid-fr (MEDIAN) and percent-check (PERCENTILE)
    // If found in the column list, we need a CTE wrapper.
    bool hasMidFr = false;
    bool hasPercentCheck = false;
    const FunctionCall* midFrCall = nullptr;
    const FunctionCall* percentCall = nullptr;

    for (auto& col : s->columns) {
        if (auto* fc = dynamic_cast<const FunctionCall*>(col.get())) {
            if (fc->name == "mid-fr") { hasMidFr = true; midFrCall = fc; }
            if (fc->name == "percent-check") { hasPercentCheck = true; percentCall = fc; }
        }
    }

    // Check for biggest-W / biggest-L in column list
    const FunctionCall* biggestWCall = nullptr;
    const FunctionCall* biggestLCall = nullptr;
    for (auto& col : s->columns) {
        if (auto* fc = dynamic_cast<const FunctionCall*>(col.get())) {
            if (fc->name == "biggest-W") biggestWCall = fc;
            if (fc->name == "biggest-L") biggestLCall = fc;
        }
    }

    // Set context for nested codegen
    currentFromTable = s->fromTable;
    currentFromAlias = s->fromAlias;

    // Generate the WHERE clause (shared)
    std::string whereClause;
    if (s->where) {
        whereClause = " WHERE " + genExpr(s->where.get());
    }

    // Build FROM + JOINs
    std::string fromClause;
    {
        std::string ref = s->fromTable;
        if (!s->fromAlias.empty()) ref += " AS " + s->fromAlias;
        fromClause = ref;
        for (auto& j : s->joins) {
            std::string jtype;
            switch (j.type) {
                case JoinType::INNER: jtype = " JOIN "; break;
                case JoinType::LEFT:  jtype = " LEFT JOIN "; break;
                case JoinType::CROSS: jtype = " CROSS JOIN "; break;
            }
            fromClause += jtype + j.table;
            if (!j.alias.empty()) fromClause += " AS " + j.alias;
            if (j.condition) fromClause += " ON " + genExpr(j.condition.get());
        }
    }

    // Handle mid-fr (MEDIAN) - rewrite as CTE
    if (hasMidFr && midFrCall && !midFrCall->args.empty()) {
        return genMedian(midFrCall, s);
    }

    // Handle percent-check (PERCENTILE) - rewrite as CTE
    if (hasPercentCheck && percentCall && percentCall->args.size() >= 2) {
        return genPercentile(percentCall, s);
    }

    // Handle biggest-W / biggest-L
    if (biggestWCall || biggestLCall) {
        std::string colExpr;
        bool isDesc = (biggestWCall != nullptr);
        const FunctionCall* fc = biggestWCall ? biggestWCall : biggestLCall;
        if (!fc->args.empty()) {
            colExpr = genExpr(fc->args[0].get());
        }

        std::ostringstream out;
        out << "SELECT * FROM " << fromClause;
        if (!whereClause.empty()) out << whereClause;
        if (!colExpr.empty()) {
            out << " ORDER BY " << colExpr << (isDesc ? " DESC" : " ASC");
        }
        out << " LIMIT 1";
        return out.str();
    }

    // Standard SELECT
    std::ostringstream out;
    out << "SELECT";
    if (s->distinct) out << " DISTINCT";

    // Columns
    std::vector<std::string> cols;
    for (auto& col : s->columns) {
        std::string colStr = genExpr(col.get());
        // Get alias if any
        std::string alias;
        if (auto* cr = dynamic_cast<const ColumnRef*>(col.get())) alias = cr->alias;
        else if (auto* fc = dynamic_cast<const FunctionCall*>(col.get())) alias = fc->alias;
        else if (auto* bo = dynamic_cast<const BinaryOp*>(col.get())) alias = bo->alias;
        else if (auto* uo = dynamic_cast<const UnaryOp*>(col.get())) alias = uo->alias;
        else if (auto* wf = dynamic_cast<const WindowFunc*>(col.get())) alias = wf->alias;
        else if (auto* wc = dynamic_cast<const Wildcard*>(col.get())) alias = wc->alias;

        if (!alias.empty()) colStr += " AS " + alias;
        cols.push_back(colStr);
    }
    out << " " << joinWith(cols, ", ");

    out << " FROM " << fromClause;

    if (!whereClause.empty()) out << whereClause;

    if (!s->groupBy.empty()) {
        out << " GROUP BY ";
        std::vector<std::string> gbs;
        for (auto& g : s->groupBy) gbs.push_back(genExpr(g.get()));
        out << joinWith(gbs, ", ");
    }

    if (s->having) {
        out << " HAVING " << genExpr(s->having.get());
    }

    if (!s->orderBy.empty()) {
        out << " ORDER BY ";
        std::vector<std::string> obs;
        for (auto& oi : s->orderBy) obs.push_back(genOrderItem(oi));
        out << joinWith(obs, ", ");
    }

    if (s->limit) {
        out << " LIMIT " << genExpr(s->limit.get());
    }

    if (s->offset) {
        out << " OFFSET " << genExpr(s->offset.get());
    }

    return out.str();
}

// -----------------------------------------------------------------------
// MEDIAN CTE
// -----------------------------------------------------------------------
std::string CodeGen::genMedian(const FunctionCall* fc, const SelectStmt* s) {
    std::string col = genExpr(fc->args[0].get());

    std::string fromClause = s->fromTable;
    if (!s->fromAlias.empty()) fromClause += " AS " + s->fromAlias;
    for (auto& j : s->joins) {
        std::string jtype;
        switch (j.type) {
            case JoinType::INNER: jtype = " JOIN "; break;
            case JoinType::LEFT:  jtype = " LEFT JOIN "; break;
            case JoinType::CROSS: jtype = " CROSS JOIN "; break;
        }
        fromClause += jtype + j.table;
        if (!j.alias.empty()) fromClause += " AS " + j.alias;
        if (j.condition) fromClause += " ON " + genExpr(j.condition.get());
    }

    std::string whereClause;
    if (s->where) whereClause = " WHERE " + genExpr(s->where.get());

    // Build alias if present
    std::string alias = fc->alias;

    std::ostringstream out;
    out << "WITH __data AS (SELECT " << col << " FROM " << fromClause << whereClause << "), ";
    out << "__ordered AS (SELECT " << col << ", ";
    out << "ROW_NUMBER() OVER (ORDER BY " << col << ") AS rn, ";
    out << "COUNT(*) OVER () AS cnt ";
    out << "FROM __data) ";
    out << "SELECT AVG(" << col << ")";
    if (!alias.empty()) out << " AS " << alias;
    out << " FROM __ordered WHERE rn IN ((cnt + 1) / 2, (cnt + 2) / 2)";

    return out.str();
}

// -----------------------------------------------------------------------
// PERCENTILE CTE
// -----------------------------------------------------------------------
std::string CodeGen::genPercentile(const FunctionCall* fc, const SelectStmt* s) {
    std::string col = genExpr(fc->args[0].get());
    std::string pct = genExpr(fc->args[1].get());

    std::string fromClause = s->fromTable;
    if (!s->fromAlias.empty()) fromClause += " AS " + s->fromAlias;
    for (auto& j : s->joins) {
        std::string jtype;
        switch (j.type) {
            case JoinType::INNER: jtype = " JOIN "; break;
            case JoinType::LEFT:  jtype = " LEFT JOIN "; break;
            case JoinType::CROSS: jtype = " CROSS JOIN "; break;
        }
        fromClause += jtype + j.table;
        if (!j.alias.empty()) fromClause += " AS " + j.alias;
        if (j.condition) fromClause += " ON " + genExpr(j.condition.get());
    }

    std::string whereClause;
    if (s->where) whereClause = " WHERE " + genExpr(s->where.get());

    std::string alias = fc->alias;

    std::ostringstream out;
    out << "WITH __data AS (SELECT " << col << " FROM " << fromClause << whereClause << "), ";
    out << "__cnt AS (SELECT COUNT(*) AS n FROM __data), ";
    out << "__ranked AS (SELECT " << col << ", ROW_NUMBER() OVER (ORDER BY " << col << ") AS rn FROM __data) ";
    out << "SELECT " << col;
    if (!alias.empty()) out << " AS " << alias;
    out << " FROM __ranked, __cnt ";
    out << "WHERE rn = CAST(CEIL(n * " << pct << " / 100.0) AS INTEGER)";

    return out.str();
}

// -----------------------------------------------------------------------
// INSERT
// -----------------------------------------------------------------------
std::string CodeGen::genInsert(const InsertStmt* s) {
    std::ostringstream out;
    out << "INSERT INTO " << s->table;

    if (!s->columns.empty()) {
        out << " (" << joinWith(s->columns, ", ") << ")";
    }

    out << " VALUES ";

    std::vector<std::string> rows;
    for (auto& row : s->valueRows) {
        std::vector<std::string> vals;
        for (auto& v : row) vals.push_back(genExpr(v.get()));
        rows.push_back("(" + joinWith(vals, ", ") + ")");
    }
    out << joinWith(rows, ", ");

    return out.str();
}

// -----------------------------------------------------------------------
// UPDATE
// -----------------------------------------------------------------------
std::string CodeGen::genUpdate(const UpdateStmt* s) {
    std::ostringstream out;
    out << "UPDATE " << s->table << " SET ";

    std::vector<std::string> assignments;
    for (auto& a : s->sets) {
        assignments.push_back(a.column + " = " + genExpr(a.value.get()));
    }
    out << joinWith(assignments, ", ");

    if (s->where) {
        out << " WHERE " << genExpr(s->where.get());
    }

    return out.str();
}

// -----------------------------------------------------------------------
// DELETE
// -----------------------------------------------------------------------
std::string CodeGen::genDelete(const DeleteStmt* s) {
    std::ostringstream out;
    out << "DELETE FROM " << s->table;
    if (s->where) {
        out << " WHERE " << genExpr(s->where.get());
    }
    return out.str();
}

// -----------------------------------------------------------------------
// CREATE TABLE
// -----------------------------------------------------------------------
std::string CodeGen::genCreate(const CreateStmt* s) {
    std::ostringstream out;
    out << "CREATE TABLE";
    if (s->ifNotExists) out << " IF NOT EXISTS";
    out << " " << s->table << " (";

    std::vector<std::string> cols;
    for (auto& cd : s->columns) {
        std::string def = cd.name + " " + cd.type;
        if (cd.primary_key) def += " PRIMARY KEY";
        if (cd.not_null) def += " NOT NULL";
        if (!cd.fk_table.empty()) {
            def += " REFERENCES " + cd.fk_table + "(" + cd.fk_col + ")";
        }
        cols.push_back(def);
    }
    out << joinWith(cols, ", ");
    out << ")";
    return out.str();
}

// -----------------------------------------------------------------------
// DROP TABLE
// -----------------------------------------------------------------------
std::string CodeGen::genDrop(const DropStmt* s) {
    std::ostringstream out;
    out << "DROP TABLE";
    if (s->ifExists) out << " IF EXISTS";
    out << " " << s->table;
    return out.str();
}

// -----------------------------------------------------------------------
// Expression generator
// -----------------------------------------------------------------------
std::string CodeGen::genExpr(const ASTNode* node) {
    if (!node) return "";

    if (auto* lit = dynamic_cast<const Literal*>(node)) {
        switch (lit->kind) {
            case LiteralKind::INT:    return std::to_string(lit->ival);
            case LiteralKind::FLOAT:  {
                std::ostringstream ss;
                ss << lit->fval;
                return ss.str();
            }
            case LiteralKind::STRING: return quoteString(lit->sval);
            case LiteralKind::NUL:    return "NULL";
            case LiteralKind::BOOL:   return lit->bval ? "1" : "0";
        }
    }

    if (auto* cr = dynamic_cast<const ColumnRef*>(node)) {
        std::string result;
        if (!cr->table.empty()) result = cr->table + ".";
        result += cr->column;
        return result;
    }

    if (auto* wc = dynamic_cast<const Wildcard*>(node)) {
        if (!wc->table.empty()) return wc->table + ".*";
        return "*";
    }

    if (auto* bo = dynamic_cast<const BinaryOp*>(node)) {
        std::string left = genExpr(bo->left.get());
        std::string right = genExpr(bo->right.get());
        std::string op = bo->op;

        // Map internal op names to SQL
        if (op == "AND") op = "AND";
        else if (op == "OR") op = "OR";

        return "(" + left + " " + op + " " + right + ")";
    }

    if (auto* uo = dynamic_cast<const UnaryOp*>(node)) {
        std::string operand = genExpr(uo->operand.get());
        if (uo->op == "NOT") return "(NOT " + operand + ")";
        return "(" + uo->op + operand + ")";
    }

    if (auto* fc = dynamic_cast<const FunctionCall*>(node)) {
        return genFunctionCall(fc);
    }

    if (auto* wf = dynamic_cast<const WindowFunc*>(node)) {
        return genWindowFunc(wf);
    }

    if (auto* sel = dynamic_cast<const SelectStmt*>(node)) {
        // Subquery
        return "(" + genSelect(sel) + ")";
    }

    throw std::runtime_error("CodeGen: Unknown expression node type");
}

// -----------------------------------------------------------------------
// Function call code generation
// -----------------------------------------------------------------------
std::string CodeGen::genFunctionCall(const FunctionCall* fc) {
    const std::string& name = fc->name;

    // Map SkibidiQL function names to SQL
    std::string sqlName;
    if (name == "headcount") sqlName = "COUNT";
    else if (name == "stack") sqlName = "SUM";
    else if (name == "mid") sqlName = "AVG";
    else if (name == "goat") sqlName = "MAX";
    else if (name == "L") sqlName = "MIN";
    else if (name == "mid-fr" || name == "percent-check" ||
             name == "biggest-W" || name == "biggest-L") {
        // These need context (handled at select level), but if used in isolation:
        sqlName = name; // will generate an error if not handled by select
    }
    else {
        // Pass through any unknown function name (user-defined)
        sqlName = name;
    }

    std::ostringstream out;
    out << sqlName << "(";

    if (fc->distinct) out << "DISTINCT ";

    std::vector<std::string> args;
    for (auto& arg : fc->args) args.push_back(genExpr(arg.get()));
    out << joinWith(args, ", ");
    out << ")";

    return out.str();
}

// -----------------------------------------------------------------------
// Window function code generation (era)
// -----------------------------------------------------------------------
std::string CodeGen::genWindowFunc(const WindowFunc* wf) {
    std::ostringstream out;
    out << wf->funcName << "() OVER (";

    bool needSpace = false;
    if (!wf->partition_by.empty()) {
        out << "PARTITION BY ";
        std::vector<std::string> parts;
        for (auto& p : wf->partition_by) parts.push_back(genExpr(p.get()));
        out << joinWith(parts, ", ");
        needSpace = true;
    }

    if (!wf->order_by.empty()) {
        if (needSpace) out << " ";
        out << "ORDER BY ";
        std::vector<std::string> obs;
        for (auto& oi : wf->order_by) obs.push_back(genOrderItem(oi));
        out << joinWith(obs, ", ");
    }

    out << ")";
    return out.str();
}

// -----------------------------------------------------------------------
// Order item
// -----------------------------------------------------------------------
std::string CodeGen::genOrderItem(const OrderItem& oi) {
    std::string result = genExpr(oi.expr.get());
    result += oi.asc ? " ASC" : " DESC";
    return result;
}
