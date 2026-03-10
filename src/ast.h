#pragma once
#include <string>
#include <vector>
#include <memory>
#include <ostream>

// Forward declarations
struct ASTVisitor;
struct ASTNode;
struct Literal;
struct ColumnRef;
struct Wildcard;
struct BinaryOp;
struct UnaryOp;
struct FunctionCall;
struct WindowFunc;
struct SelectStmt;
struct InsertStmt;
struct UpdateStmt;
struct DeleteStmt;
struct CreateStmt;
struct DropStmt;

// -----------------------------------------------------------------------
// Visitor base
// -----------------------------------------------------------------------
struct ASTVisitor {
    virtual ~ASTVisitor() = default;
    virtual void visit(Literal&) = 0;
    virtual void visit(ColumnRef&) = 0;
    virtual void visit(Wildcard&) = 0;
    virtual void visit(BinaryOp&) = 0;
    virtual void visit(UnaryOp&) = 0;
    virtual void visit(FunctionCall&) = 0;
    virtual void visit(WindowFunc&) = 0;
    virtual void visit(SelectStmt&) = 0;
    virtual void visit(InsertStmt&) = 0;
    virtual void visit(UpdateStmt&) = 0;
    virtual void visit(DeleteStmt&) = 0;
    virtual void visit(CreateStmt&) = 0;
    virtual void visit(DropStmt&) = 0;
};

// -----------------------------------------------------------------------
// Base node
// -----------------------------------------------------------------------
struct ASTNode {
    int line = 0, col = 0;
    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor&) = 0;
    virtual void print(std::ostream& os, int indent = 0) const = 0;
    virtual std::unique_ptr<ASTNode> clone() const = 0;

protected:
    static void printIndent(std::ostream& os, int indent) {
        for (int i = 0; i < indent; ++i) os << "  ";
    }
};

// -----------------------------------------------------------------------
// Literal
// -----------------------------------------------------------------------
enum class LiteralKind { INT, FLOAT, STRING, NUL, BOOL };

struct Literal : ASTNode {
    LiteralKind kind;
    std::string sval;   // string value (for string/null)
    long long   ival;   // integer value
    double      fval;   // float value
    bool        bval;   // bool value

    static std::unique_ptr<Literal> makeInt(long long v, int l = 0, int c = 0) {
        auto n = std::make_unique<Literal>();
        n->kind = LiteralKind::INT; n->ival = v; n->line = l; n->col = c;
        return n;
    }
    static std::unique_ptr<Literal> makeFloat(double v, int l = 0, int c = 0) {
        auto n = std::make_unique<Literal>();
        n->kind = LiteralKind::FLOAT; n->fval = v; n->line = l; n->col = c;
        return n;
    }
    static std::unique_ptr<Literal> makeString(std::string v, int l = 0, int c = 0) {
        auto n = std::make_unique<Literal>();
        n->kind = LiteralKind::STRING; n->sval = std::move(v); n->line = l; n->col = c;
        return n;
    }
    static std::unique_ptr<Literal> makeNull(int l = 0, int c = 0) {
        auto n = std::make_unique<Literal>();
        n->kind = LiteralKind::NUL; n->line = l; n->col = c;
        return n;
    }
    static std::unique_ptr<Literal> makeBool(bool v, int l = 0, int c = 0) {
        auto n = std::make_unique<Literal>();
        n->kind = LiteralKind::BOOL; n->bval = v; n->line = l; n->col = c;
        return n;
    }

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "Literal(";
        switch (kind) {
            case LiteralKind::INT:    os << ival; break;
            case LiteralKind::FLOAT:  os << fval; break;
            case LiteralKind::STRING: os << "'" << sval << "'"; break;
            case LiteralKind::NUL:    os << "NULL"; break;
            case LiteralKind::BOOL:   os << (bval ? "true" : "false"); break;
        }
        os << ")\n";
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<Literal>(*this);
        return n;
    }
};

// -----------------------------------------------------------------------
// ColumnRef
// -----------------------------------------------------------------------
struct ColumnRef : ASTNode {
    std::string table;  // optional table qualifier
    std::string column;
    std::string alias;  // optional alias (lowkey)

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "ColumnRef(";
        if (!table.empty()) os << table << ".";
        os << column;
        if (!alias.empty()) os << " AS " << alias;
        os << ")\n";
    }
    std::unique_ptr<ASTNode> clone() const override {
        return std::make_unique<ColumnRef>(*this);
    }
};

// -----------------------------------------------------------------------
// Wildcard
// -----------------------------------------------------------------------
struct Wildcard : ASTNode {
    std::string table; // optional table qualifier for t.*
    std::string alias;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        if (!table.empty()) os << "Wildcard(" << table << ".*)\n";
        else os << "Wildcard(*)\n";
    }
    std::unique_ptr<ASTNode> clone() const override {
        return std::make_unique<Wildcard>(*this);
    }
};

// -----------------------------------------------------------------------
// BinaryOp
// -----------------------------------------------------------------------
struct BinaryOp : ASTNode {
    std::string op;
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
    std::string alias;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "BinaryOp(" << op << ")\n";
        if (left) left->print(os, indent + 1);
        if (right) right->print(os, indent + 1);
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<BinaryOp>();
        n->op = op;
        n->alias = alias;
        n->line = line; n->col = col;
        if (left) n->left = left->clone();
        if (right) n->right = right->clone();
        return n;
    }
};

// -----------------------------------------------------------------------
// UnaryOp
// -----------------------------------------------------------------------
struct UnaryOp : ASTNode {
    std::string op;
    std::unique_ptr<ASTNode> operand;
    std::string alias;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "UnaryOp(" << op << ")\n";
        if (operand) operand->print(os, indent + 1);
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<UnaryOp>();
        n->op = op;
        n->alias = alias;
        n->line = line; n->col = col;
        if (operand) n->operand = operand->clone();
        return n;
    }
};

// -----------------------------------------------------------------------
// FunctionCall
// -----------------------------------------------------------------------
struct FunctionCall : ASTNode {
    std::string name;       // function name (headcount, stack, mid, goat, L, biggest-W, biggest-L, mid-fr, percent-check)
    std::vector<std::unique_ptr<ASTNode>> args;
    bool distinct = false;  // for headcount(unique-fr col)
    std::string alias;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "FunctionCall(" << name;
        if (distinct) os << " DISTINCT";
        os << ")\n";
        for (auto& a : args) if (a) a->print(os, indent + 1);
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<FunctionCall>();
        n->name = name;
        n->distinct = distinct;
        n->alias = alias;
        n->line = line; n->col = col;
        for (auto& a : args) n->args.push_back(a ? a->clone() : nullptr);
        return n;
    }
};

// -----------------------------------------------------------------------
// WindowFunc (era - RANK() OVER)
// -----------------------------------------------------------------------
struct OrderItem {
    std::unique_ptr<ASTNode> expr;
    bool asc = true;

    OrderItem() = default;
    OrderItem(const OrderItem& o) : asc(o.asc) {
        if (o.expr) expr = o.expr->clone();
    }
    OrderItem& operator=(const OrderItem& o) {
        asc = o.asc;
        if (o.expr) expr = o.expr->clone();
        else expr = nullptr;
        return *this;
    }
    OrderItem(OrderItem&&) = default;
    OrderItem& operator=(OrderItem&&) = default;
};

struct WindowFunc : ASTNode {
    std::string funcName;   // e.g. "RANK"
    std::vector<std::unique_ptr<ASTNode>> partition_by;
    std::vector<OrderItem> order_by;
    std::string alias;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "WindowFunc(" << funcName << " OVER (";
        if (!partition_by.empty()) os << "PARTITION BY ...";
        os << " ORDER BY ...))\n";
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<WindowFunc>();
        n->funcName = funcName;
        n->alias = alias;
        n->line = line; n->col = col;
        for (auto& p : partition_by) n->partition_by.push_back(p ? p->clone() : nullptr);
        for (auto& o : order_by) {
            OrderItem oi;
            oi.asc = o.asc;
            if (o.expr) oi.expr = o.expr->clone();
            n->order_by.push_back(std::move(oi));
        }
        return n;
    }
};

// -----------------------------------------------------------------------
// JoinClause
// -----------------------------------------------------------------------
enum class JoinType { INNER, LEFT, CROSS };

struct JoinClause {
    JoinType type = JoinType::INNER;
    std::string table;
    std::string alias;
    std::unique_ptr<ASTNode> condition;

    JoinClause() = default;
    JoinClause(const JoinClause& o) : type(o.type), table(o.table), alias(o.alias) {
        if (o.condition) condition = o.condition->clone();
    }
    JoinClause& operator=(const JoinClause& o) {
        type = o.type; table = o.table; alias = o.alias;
        if (o.condition) condition = o.condition->clone();
        else condition = nullptr;
        return *this;
    }
    JoinClause(JoinClause&&) = default;
    JoinClause& operator=(JoinClause&&) = default;
};

// -----------------------------------------------------------------------
// SelectStmt
// -----------------------------------------------------------------------
struct SelectStmt : ASTNode {
    bool distinct = false;
    std::vector<std::unique_ptr<ASTNode>> columns;  // select list
    std::string fromTable;
    std::string fromAlias;
    std::vector<JoinClause> joins;
    std::unique_ptr<ASTNode> where;
    std::vector<std::unique_ptr<ASTNode>> groupBy;
    std::unique_ptr<ASTNode> having;
    std::vector<OrderItem> orderBy;
    std::unique_ptr<ASTNode> limit;
    std::unique_ptr<ASTNode> offset;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "SelectStmt" << (distinct ? " DISTINCT" : "") << "\n";
        printIndent(os, indent + 1); os << "FROM: " << fromTable;
        if (!fromAlias.empty()) os << " AS " << fromAlias;
        os << "\n";
        printIndent(os, indent + 1); os << "COLUMNS:\n";
        for (auto& c : columns) if (c) c->print(os, indent + 2);
        if (where) { printIndent(os, indent + 1); os << "WHERE:\n"; where->print(os, indent + 2); }
        if (!groupBy.empty()) { printIndent(os, indent + 1); os << "GROUP BY:\n"; for (auto& g : groupBy) if (g) g->print(os, indent + 2); }
        if (having) { printIndent(os, indent + 1); os << "HAVING:\n"; having->print(os, indent + 2); }
        if (!orderBy.empty()) { printIndent(os, indent + 1); os << "ORDER BY: (count=" << orderBy.size() << ")\n"; }
        if (limit) { printIndent(os, indent + 1); os << "LIMIT:\n"; limit->print(os, indent + 2); }
        if (offset) { printIndent(os, indent + 1); os << "OFFSET:\n"; offset->print(os, indent + 2); }
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<SelectStmt>();
        n->distinct = distinct;
        n->fromTable = fromTable;
        n->fromAlias = fromAlias;
        n->line = line; n->col = col;
        for (auto& c : columns) n->columns.push_back(c ? c->clone() : nullptr);
        for (auto& j : joins) n->joins.push_back(j);
        if (where) n->where = where->clone();
        for (auto& g : groupBy) n->groupBy.push_back(g ? g->clone() : nullptr);
        if (having) n->having = having->clone();
        for (auto& o : orderBy) {
            OrderItem oi;
            oi.asc = o.asc;
            if (o.expr) oi.expr = o.expr->clone();
            n->orderBy.push_back(std::move(oi));
        }
        if (limit) n->limit = limit->clone();
        if (offset) n->offset = offset->clone();
        return n;
    }
};

// -----------------------------------------------------------------------
// InsertStmt
// -----------------------------------------------------------------------
struct InsertStmt : ASTNode {
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::vector<std::unique_ptr<ASTNode>>> valueRows;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "InsertStmt(INTO " << table << ")\n";
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<InsertStmt>();
        n->table = table;
        n->columns = columns;
        n->line = line; n->col = col;
        for (auto& row : valueRows) {
            std::vector<std::unique_ptr<ASTNode>> r;
            for (auto& v2 : row) r.push_back(v2 ? v2->clone() : nullptr);
            n->valueRows.push_back(std::move(r));
        }
        return n;
    }
};

// -----------------------------------------------------------------------
// UpdateStmt
// -----------------------------------------------------------------------
struct Assignment {
    std::string column;
    std::unique_ptr<ASTNode> value;

    Assignment() = default;
    Assignment(const Assignment& o) : column(o.column) {
        if (o.value) value = o.value->clone();
    }
    Assignment& operator=(const Assignment& o) {
        column = o.column;
        if (o.value) value = o.value->clone();
        else value = nullptr;
        return *this;
    }
    Assignment(Assignment&&) = default;
    Assignment& operator=(Assignment&&) = default;
};

struct UpdateStmt : ASTNode {
    std::string table;
    std::vector<Assignment> sets;
    std::unique_ptr<ASTNode> where;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "UpdateStmt(" << table << ")\n";
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<UpdateStmt>();
        n->table = table;
        n->line = line; n->col = col;
        for (auto& s : sets) n->sets.push_back(s);
        if (where) n->where = where->clone();
        return n;
    }
};

// -----------------------------------------------------------------------
// DeleteStmt
// -----------------------------------------------------------------------
struct DeleteStmt : ASTNode {
    std::string table;
    std::unique_ptr<ASTNode> where;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "DeleteStmt(FROM " << table << ")\n";
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<DeleteStmt>();
        n->table = table;
        n->line = line; n->col = col;
        if (where) n->where = where->clone();
        return n;
    }
};

// -----------------------------------------------------------------------
// CreateStmt
// -----------------------------------------------------------------------
struct ColumnDef {
    std::string name;
    std::string type;       // INTEGER, TEXT, REAL, BLOB
    bool primary_key = false;
    bool not_null = false;
    std::string fk_table;
    std::string fk_col;
};

struct CreateStmt : ASTNode {
    std::string table;
    std::vector<ColumnDef> columns;
    bool ifNotExists = false;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "CreateStmt(" << table << ")\n";
        for (auto& c : columns) {
            printIndent(os, indent + 1);
            os << "  " << c.name << " " << c.type;
            if (c.primary_key) os << " PK";
            if (c.not_null) os << " NOT NULL";
            if (!c.fk_table.empty()) os << " FK(" << c.fk_table << "." << c.fk_col << ")";
            os << "\n";
        }
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<CreateStmt>();
        n->table = table;
        n->columns = columns;
        n->ifNotExists = ifNotExists;
        n->line = line; n->col = col;
        return n;
    }
};

// -----------------------------------------------------------------------
// DropStmt
// -----------------------------------------------------------------------
struct DropStmt : ASTNode {
    std::string table;
    bool ifExists = false;

    void accept(ASTVisitor& v) override { v.visit(*this); }
    void print(std::ostream& os, int indent) const override {
        printIndent(os, indent);
        os << "DropStmt(" << table << ")\n";
    }
    std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<DropStmt>();
        n->table = table;
        n->ifExists = ifExists;
        n->line = line; n->col = col;
        return n;
    }
};
