#pragma once

#include "ast.h"
#include "metadata.h"
#include "native_index.h"
#include "native_storage.h"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

struct NativeQueryResult {
    std::vector<std::string> columns;
    std::vector<Tuple> rows;
    std::size_t rowsAffected = 0;
    std::string message;
};

struct NativeEngineStats {
    std::size_t residentPages = 0;
    std::size_t tableScans = 0;
    std::size_t rowsRead = 0;
    std::size_t rowsWritten = 0;
    std::size_t nestedLoopComparisons = 0;
    std::size_t hashJoinProbes = 0;
    std::size_t indexLookups = 0;
    std::size_t vectorizedQueries = 0;
    std::size_t vectorBatches = 0;
    std::size_t vectorRows = 0;
    std::size_t decodedColumns = 0;
    std::size_t skippedColumns = 0;
    std::size_t vectorNulls = 0;
    std::size_t joinPlansEnumerated = 0;
    std::size_t joinOrderChanges = 0;
    double estimatedJoinCost = 0.0;
};

class NativeEngine {
public:
    explicit NativeEngine(std::filesystem::path databasePath,
                          std::size_t bufferPages = 128);
    ~NativeEngine();

    NativeEngine(const NativeEngine&) = delete;
    NativeEngine& operator=(const NativeEngine&) = delete;

    Catalog& catalog() { return catalog_; }
    const Catalog& catalog() const { return catalog_; }

    NativeQueryResult execute(const ASTNode* statement);
    void beginTransaction();
    void commitTransaction();
    void rollbackTransaction();
    bool transactionActive() const { return transactionActive_; }
    void flush();
    NativeEngineStats stats() const;
    void resetStats();

private:
    struct BoundColumn {
        std::string table;
        std::string alias;
        std::string name;
    };

    struct EvalRow {
        std::shared_ptr<const std::vector<BoundColumn>> columns;
        Tuple values;
    };

    struct ScanRow {
        RowId id;
        EvalRow row;
    };

    struct OutputRow {
        Tuple values;
        std::vector<Value> orderKeys;
    };

    struct TypedVector {
        using Storage = std::variant<
            std::monostate,
            std::vector<std::int64_t>,
            std::vector<double>,
            std::vector<std::string>,
            std::vector<std::uint8_t>,
            std::vector<Value::Blob>>;

        ValueType type = ValueType::Null;
        std::size_t size = 0;
        std::vector<std::uint64_t> nullBitmap;
        Storage storage;

        explicit TypedVector(ValueType valueType = ValueType::Null);
        void reserve(std::size_t capacity);
        void append(const Value& value);
        bool isNull(std::size_t index) const;
        Value value(std::size_t index) const;
    };

    using VectorPtr = std::shared_ptr<TypedVector>;

    struct VectorBatch {
        std::vector<BoundColumn> columns;
        std::vector<VectorPtr> values;
        std::size_t rowCount = 0;
    };

    struct RelationData {
        std::string table;
        std::string alias;
        std::vector<EvalRow> rows;
        std::unordered_map<std::string, std::size_t> distinctCounts;
    };

    struct TableStatistics {
        std::size_t rowCount = 0;
        std::unordered_map<std::string, std::size_t> distinctCounts;
    };

    std::filesystem::path root_;
    bool temporary_ = false;
    Catalog catalog_;
    BufferPool bufferPool_;
    std::unordered_map<std::string, std::unique_ptr<BPlusTree>>
        primaryIndexes_;
    std::unordered_map<std::string, TableStatistics> tableStatistics_;
    mutable NativeEngineStats stats_;
    bool transactionActive_ = false;
    std::unordered_map<std::string, std::vector<std::uint8_t>>
        transactionSnapshot_;

    NativeQueryResult executeCreate(const CreateStmt& statement);
    NativeQueryResult executeDrop(const DropStmt& statement);
    NativeQueryResult executeInsert(const InsertStmt& statement);
    NativeQueryResult executeUpdate(const UpdateStmt& statement);
    NativeQueryResult executeDelete(const DeleteStmt& statement);
    NativeQueryResult executeSelect(const SelectStmt& statement);
    std::optional<NativeQueryResult> executeVectorizedSelect(
        const SelectStmt& statement);
    std::optional<std::vector<EvalRow>> executeCostBasedJoins(
        const SelectStmt& statement);

    std::filesystem::path tablePath(const std::string& table) const;
    HeapFile heap(const std::string& table);
    std::shared_ptr<const std::vector<BoundColumn>> boundSchema(
        const std::string& table,
        const std::string& alias) const;
    static std::shared_ptr<const std::vector<BoundColumn>> combineSchemas(
        const std::shared_ptr<const std::vector<BoundColumn>>& left,
        const std::shared_ptr<const std::vector<BoundColumn>>& right);
    std::vector<ScanRow> scanTable(const std::string& table,
                                   const std::string& alias);
    std::vector<ScanRow> lookupPrimaryKey(const std::string& table,
                                          const std::string& alias,
                                          const Value& key);
    BPlusTree& primaryIndex(const std::string& table);
    std::optional<Value> primaryKeyPredicate(
        const std::string& table,
        const std::string& alias,
        const ASTNode* expression) const;
    Value eval(const ASTNode* expression, const EvalRow& row) const;
    VectorPtr evalVector(const ASTNode* expression,
                         const VectorBatch& batch) const;
    std::size_t resolveVectorColumn(const ColumnRef& column,
                                    const VectorBatch& batch) const;
    bool canVectorize(const SelectStmt& statement) const;
    static ValueType declaredValueType(const ColumnMeta& column);
    Value evalGrouped(const ASTNode* expression,
                      const std::vector<EvalRow>& rows) const;
    Value aggregate(const FunctionCall& function,
                    const std::vector<EvalRow>& rows) const;
    bool hasAggregate(const ASTNode* expression) const;
    bool selectHasAggregate(const SelectStmt& statement) const;
    static Value applyBinary(const std::string& op,
                             const Value& left,
                             const Value& right);
    static Value applyUnary(const std::string& op,
                            const Value& value);

    Value resolveColumn(const ColumnRef& column,
                        const EvalRow& row) const;
    Value resolveOutputColumn(const ColumnRef& column,
                              const std::vector<std::string>& names,
                              const Tuple& values) const;
    std::vector<std::string> outputNames(
        const SelectStmt& statement,
        const EvalRow* sample) const;
    Tuple project(const SelectStmt& statement,
                  const EvalRow& row,
                  const std::vector<EvalRow>* allRows = nullptr) const;
    Tuple projectGrouped(const SelectStmt& statement,
                         const std::vector<EvalRow>& rows) const;
    std::vector<Value> orderKeys(const SelectStmt& statement,
                                 const EvalRow& row,
                                 const std::vector<std::string>& names,
                                 const Tuple& projected) const;
    std::vector<Value> orderKeysGrouped(
        const SelectStmt& statement,
        const std::vector<EvalRow>& rows,
        const std::vector<std::string>& names,
        const Tuple& projected) const;

    Tuple validateAndCoerce(const TableMeta& table,
                            Tuple tuple,
                            const std::optional<RowId>& replacing);
    void validateForeignKeys(const TableMeta& table,
                             const Tuple& tuple);
    void validateNoIncomingReferences(const TableMeta& table,
                                      const Tuple& tuple,
                                      const Tuple* replacement = nullptr);
    static Value coerceValue(const ColumnMeta& column,
                             const Value& value);
    Value windowValue(const WindowFunc& window,
                      const EvalRow& row,
                      const std::vector<EvalRow>& allRows) const;
    static bool tupleEqual(const Tuple& left, const Tuple& right);
    static std::size_t tupleHash(const Tuple& tuple);

    std::vector<std::uint8_t> readFile(
        const std::filesystem::path& path) const;
    void restoreFile(const std::filesystem::path& path,
                     const std::optional<std::vector<std::uint8_t>>& bytes);
    void reloadCatalog();
};
