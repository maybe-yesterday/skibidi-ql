#include "native_engine.h"

#include "codegen.h"
#include "hash_utils.h"
#include "native_raw.h"
#include "skibidi_config.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {

struct TupleKeyHash {
    std::size_t operator()(const Tuple& tuple) const {
        std::size_t seed = 0;
        for (const auto& value : tuple) {
            seed = skibidi::hash::combine(seed, value.hash());
        }
        return seed;
    }
};

struct TupleKeyEqual {
    bool operator()(const Tuple& left, const Tuple& right) const {
        return left == right;
    }
};

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::toupper(ch));
                   });
    return value;
}

std::string expressionAlias(const ASTNode* expression,
                            std::size_t fallback) {
    if (auto* column = dynamic_cast<const ColumnRef*>(expression)) {
        return column->alias.empty() ? column->column : column->alias;
    }
    if (auto* function = dynamic_cast<const FunctionCall*>(expression)) {
        return function->alias.empty() ? function->name : function->alias;
    }
    if (auto* binary = dynamic_cast<const BinaryOp*>(expression)) {
        if (!binary->alias.empty()) return binary->alias;
    }
    if (auto* unary = dynamic_cast<const UnaryOp*>(expression)) {
        if (!unary->alias.empty()) return unary->alias;
    }
    if (auto* window = dynamic_cast<const WindowFunc*>(expression)) {
        return window->alias.empty() ? window->funcName : window->alias;
    }
    return "expr" + std::to_string(fallback + 1);
}

std::size_t estimateValueBytes(const Value& value) {
    std::size_t bytes = sizeof(Value);
    if (value.type() == ValueType::Text) {
        bytes += value.asText().capacity();
    } else if (value.type() == ValueType::Blob) {
        bytes += value.asBlob().capacity();
    }
    return bytes;
}

std::size_t estimateTupleBytes(const Tuple& tuple) {
    std::size_t bytes = sizeof(Tuple) +
        tuple.capacity() * sizeof(Value);
    for (const auto& value : tuple) {
        bytes += estimateValueBytes(value);
    }
    return bytes;
}

std::size_t estimateResultBytes(const NativeQueryResult& result) {
    std::size_t bytes = sizeof(NativeQueryResult) +
        result.columns.capacity() * sizeof(std::string) +
        result.rows.capacity() * sizeof(Tuple) +
        result.message.capacity();
    for (const auto& column : result.columns) {
        bytes += column.capacity();
    }
    for (const auto& row : result.rows) {
        bytes += estimateTupleBytes(row);
    }
    return bytes;
}

std::string rowIdString(RowId row) {
    return std::to_string(row.page) + ":" + std::to_string(row.slot);
}

RowId parseRowIdString(const std::string& text) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) {
        throw std::runtime_error("Malformed snapshot row id: " + text);
    }
    const auto page = std::stoul(text.substr(0, colon));
    const auto slot = std::stoul(text.substr(colon + 1));
    return RowId{static_cast<std::uint32_t>(page),
                 static_cast<std::uint16_t>(slot)};
}

std::uint64_t snapshotHash(std::uint64_t seed,
                           std::uint64_t epoch,
                           const std::string& key) {
    return skibidi::hash::avalanche64(
        seed ^ (epoch * skibidi::hash::kGoldenRatio64) ^
        skibidi::hash::fnv1a64(key));
}

std::string splitForKey(std::uint64_t seed, const std::string& key) {
    const auto bucket =
        snapshotHash(seed, 0, key) %
        skibidi::config::kSnapshotSplitBuckets;
    if (bucket < skibidi::config::kDefaultTrainSplitPercent) {
        return "train";
    }
    if (bucket < skibidi::config::kDefaultTrainSplitPercent +
                     skibidi::config::kDefaultValidationSplitPercent) {
        return "validation";
    }
    return "test";
}

std::vector<const SnapshotRowMeta*> plannedSnapshotRows(
    const DatasetSnapshotMeta& snapshot,
    std::uint64_t epoch,
    std::uint64_t rank,
    std::uint64_t worldSize,
    const std::string& split = "train") {
    if (worldSize == 0) {
        throw std::runtime_error("world-size must be greater than zero");
    }
    if (rank >= worldSize) {
        throw std::runtime_error("rank must be smaller than world-size");
    }

    std::vector<const SnapshotRowMeta*> rows;
    rows.reserve(snapshot.rows.size());
    for (const auto& row : snapshot.rows) {
        if (row.split == split) rows.push_back(&row);
    }
    std::stable_sort(rows.begin(), rows.end(),
        [&](const SnapshotRowMeta* left,
            const SnapshotRowMeta* right) {
        const auto leftHash =
            snapshotHash(snapshot.seed, epoch, left->rowid);
        const auto rightHash =
            snapshotHash(snapshot.seed, epoch, right->rowid);
        if (leftHash != rightHash) return leftHash < rightHash;
        return left->rowid < right->rowid;
    });

    std::vector<const SnapshotRowMeta*> sharded;
    sharded.reserve((rows.size() + static_cast<std::size_t>(worldSize) - 1) /
                    static_cast<std::size_t>(worldSize));
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (index % worldSize == rank) sharded.push_back(rows[index]);
    }
    return sharded;
}

std::string joinStrings(const std::vector<std::string>& values,
                        const std::string& separator) {
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) out += separator;
        out += values[index];
    }
    return out;
}

void addKeyValue(NativeQueryResult& result,
                 const std::string& field,
                 const std::string& value) {
    result.rows.push_back(
        Tuple{Value(field), Value(value)});
}

std::optional<std::size_t> tableColumnIndex(const TableMeta& table,
                                            const std::string& column) {
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        if (table.columns[index].name == column) return index;
    }
    return std::nullopt;
}

} // namespace

NativeEngine::NativeEngine(std::filesystem::path databasePath,
                           std::size_t bufferPages)
    : root_(std::move(databasePath)),
      catalog_((root_ / "catalog.json").string()),
      bufferPool_(bufferPages),
      wal_(root_) {
    if (root_ == ":memory:") {
        temporary_ = true;
        const auto stamp =
            std::chrono::high_resolution_clock::now()
                .time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("skibidi-native-" + std::to_string(stamp));
        catalog_ = Catalog((root_ / "catalog.json").string());
        wal_.resetRoot(root_);
    }
    root_ = std::filesystem::absolute(root_).lexically_normal();
    catalog_ = Catalog((root_ / "catalog.json").string());
    wal_.resetRoot(root_);
    auto startupLocks = NativeLockManager::global().acquireAll({
        {root_.generic_string() + "|!database",
         NativeLockMode::Exclusive},
    });
    std::filesystem::create_directories(root_ / "tables");
    wal_.recover();
    catalog_.load();
}

NativeEngine::~NativeEngine() {
    try {
        if (transactionActive_) rollbackTransaction();
        else flush();
        if (temporary_) {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }
    } catch (...) {
    }
}

void NativeEngine::beginTransaction() {
    std::lock_guard<std::recursive_mutex> guard(operationMutex_);
    if (transactionActive_) {
        throw std::runtime_error("A transaction is already active");
    }

    transactionLocks_ = NativeLockManager::global().acquireAll({
        {root_.generic_string() + "|!database",
         NativeLockMode::Exclusive},
    });

    try {
        flushUnlocked();
        transactionSnapshot_.clear();
        if (std::filesystem::exists(root_)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(root_)) {
                if (!entry.is_regular_file()) continue;
                const auto relative =
                    std::filesystem::relative(entry.path(), root_)
                        .generic_string();
                transactionSnapshot_[relative] = readFile(entry.path());
            }
        }
        beginWalTransaction();
        transactionActive_ = true;
    } catch (...) {
        walTransactionId_ = 0;
        walLoggedFiles_.clear();
        transactionSnapshot_.clear();
        transactionLocks_.clear();
        throw;
    }
}

void NativeEngine::commitTransaction() {
    std::lock_guard<std::recursive_mutex> guard(operationMutex_);
    if (!transactionActive_) {
        throw std::runtime_error("No transaction is active");
    }
    flushUnlocked();
    commitWalTransaction();
    wal_.checkpoint();
    transactionSnapshot_.clear();
    transactionActive_ = false;
    transactionLocks_.clear();
}

void NativeEngine::rollbackTransaction() {
    std::lock_guard<std::recursive_mutex> guard(operationMutex_);
    if (!transactionActive_) {
        throw std::runtime_error("No transaction is active");
    }
    bufferPool_.discardAll();
    mappedHeaps_.clear();
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    if (error) throw std::runtime_error(error.message());
    std::filesystem::create_directories(root_ / "tables");
    for (const auto& item : transactionSnapshot_) {
        const auto path = root_ / std::filesystem::path(item.first);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(item.second.data()),
                     static_cast<std::streamsize>(item.second.size()));
        if (!output) {
            throw std::runtime_error(
                "Failed restoring transaction snapshot");
        }
    }
    primaryIndexes_.clear();
    tableStatistics_.clear();
    reloadCatalog();
    transactionSnapshot_.clear();
    transactionActive_ = false;
    walTransactionId_ = 0;
    walLoggedFiles_.clear();
    wal_.checkpoint();
    transactionLocks_.clear();
}

bool NativeEngine::transactionActive() const {
    std::lock_guard<std::recursive_mutex> guard(operationMutex_);
    return transactionActive_;
}

NativeQueryResult NativeEngine::execute(const ASTNode* statement) {
    if (!statement) return {};
    std::lock_guard<std::recursive_mutex> guard(operationMutex_);
    std::vector<NativeLockGuard> locks;
    if (!transactionActive_) {
        locks = acquireStatementLocks(statement);
    }
    const bool autoWal =
        !transactionActive_ && statementMutatesPersistentState(statement);

    auto runStatement = [&]() -> NativeQueryResult {
        if (auto* create = dynamic_cast<const CreateStmt*>(statement)) {
            return executeCreate(*create);
        }
        if (auto* drop = dynamic_cast<const DropStmt*>(statement)) {
            return executeDrop(*drop);
        }
        if (auto* insert = dynamic_cast<const InsertStmt*>(statement)) {
            return executeInsert(*insert);
        }
        if (auto* update = dynamic_cast<const UpdateStmt*>(statement)) {
            return executeUpdate(*update);
        }
        if (auto* remove = dynamic_cast<const DeleteStmt*>(statement)) {
            return executeDelete(*remove);
        }
        if (auto* select = dynamic_cast<const SelectStmt*>(statement)) {
            return executeSelect(*select);
        }
        if (auto* snapshot =
                dynamic_cast<const CreateSnapshotStmt*>(statement)) {
            return executeCreateSnapshot(*snapshot);
        }
        if (auto* exportTorch =
                dynamic_cast<const ExportTorchStmt*>(statement)) {
            return executeExportTorch(*exportTorch);
        }
        if (auto* explain =
                dynamic_cast<const ExplainBatchStmt*>(statement)) {
            return executeExplainBatch(*explain);
        }
        if (auto* context =
                dynamic_cast<const CreateContextStmt*>(statement)) {
            return executeCreateContext(*context);
        }
        if (auto* memory =
                dynamic_cast<const AppendMemoryStmt*>(statement)) {
            return executeAppendMemory(*memory);
        }
        if (auto* spill =
                dynamic_cast<const SpillContextStmt*>(statement)) {
            return executeSpillContext(*spill);
        }
        if (auto* explainContext =
                dynamic_cast<const ExplainContextStmt*>(statement)) {
            return executeExplainContext(*explainContext);
        }
        if (auto* tag =
                dynamic_cast<const TagMemoryStmt*>(statement)) {
            return executeTagMemory(*tag);
        }
        if (auto* showTabs =
                dynamic_cast<const ShowTabsStmt*>(statement)) {
            return executeShowTabs(*showTabs);
        }
        if (auto* showSchemas =
                dynamic_cast<const ShowContextSchemasStmt*>(statement)) {
            return executeShowContextSchemas(*showSchemas);
        }
        if (auto* showObjects =
                dynamic_cast<const ShowContextObjectsStmt*>(statement)) {
            return executeShowContextObjects(*showObjects);
        }
        if (auto* aliasTab =
                dynamic_cast<const AliasTabStmt*>(statement)) {
            return executeAliasTab(*aliasTab);
        }
        if (auto* mergeTabs =
                dynamic_cast<const MergeTabsStmt*>(statement)) {
            return executeMergeTabs(*mergeTabs);
        }
        throw std::runtime_error("Unsupported native statement");
    };

    try {
        if (autoWal) beginWalTransaction();
        NativeQueryResult result = runStatement();
        if (autoWal) {
            flushUnlocked();
            commitWalTransaction();
            wal_.checkpoint();
        }
        return result;
    } catch (...) {
        if (autoWal) discardWalTransaction();
        throw;
    }
}

void NativeEngine::flush() {
    std::lock_guard<std::recursive_mutex> guard(operationMutex_);
    std::vector<NativeLockGuard> locks;
    if (!transactionActive_) {
        locks = NativeLockManager::global().acquireAll({
            {root_.generic_string() + "|!database",
             NativeLockMode::Exclusive},
        });
    }
    flushUnlocked();
}

NativeEngineStats NativeEngine::stats() const {
    std::lock_guard<std::recursive_mutex> guard(operationMutex_);
    auto result = stats_;
    result.residentPages = bufferPool_.residentPages();
    result.bufferCapacityPages = bufferPool_.capacityPages();
    result.bufferPageReads = bufferPool_.pageReads();
    result.bufferEvictions = bufferPool_.evictions();
    result.estimatedMemoryBytes =
        result.residentPages * SlottedPage::PAGE_SIZE;
    for (const auto& item : primaryIndexes_) {
        const auto keys = item.second ? item.second->size() : 0;
        result.estimatedMemoryBytes +=
            keys * (sizeof(Value) + sizeof(RowId) + 48);
        result.estimatedMemoryBytes +=
            (item.second ? item.second->height() : 0) *
            skibidi::config::kEstimatedIndexLevelBytes;
    }
    for (const auto& item : contextResultCache_) {
        result.estimatedMemoryBytes += item.first.capacity();
        result.estimatedMemoryBytes += estimateResultBytes(item.second);
    }
    result.estimatedMemoryBytes +=
        mappedHeaps_.size() * sizeof(MappedHeapFile);
    for (const auto& table : tableStatistics_) {
        result.estimatedMemoryBytes +=
            table.first.capacity() +
            sizeof(TableStatistics) +
            table.second.distinctCounts.size() *
                (sizeof(std::string) + sizeof(std::size_t)) +
            table.second.columnRanges.size() *
                (sizeof(std::string) +
                 sizeof(TableStatistics::ColumnRange));
        for (const auto& range : table.second.columnRanges) {
            result.estimatedMemoryBytes +=
                range.first.capacity() +
                range.second.valueCounts.size() *
                    (sizeof(Value) + sizeof(std::size_t) + 32);
        }
    }
    return result;
}

void NativeEngine::resetStats() {
    std::lock_guard<std::recursive_mutex> guard(operationMutex_);
    stats_ = {};
    bufferPool_.resetStats();
}

std::vector<NativeLockGuard> NativeEngine::acquireStatementLocks(
    const ASTNode* statement) const {
    auto scoped = [&](const std::string& resource) {
        return root_.generic_string() + "|" + resource;
    };
    auto tableResource = [&](const std::string& table) {
        return scoped("table:" + table);
    };

    std::vector<NativeLockRequest> requests;
    auto catalog = [&](NativeLockMode mode) {
        requests.push_back({scoped("catalog"), mode});
    };
    auto table = [&](const std::string& name, NativeLockMode mode) {
        if (!name.empty()) requests.push_back({tableResource(name), mode});
    };

    requests.push_back({
        scoped("!database"),
        statementMutatesPersistentState(statement)
            ? NativeLockMode::Exclusive
            : NativeLockMode::Shared,
    });

    if (auto* select = dynamic_cast<const SelectStmt*>(statement)) {
        catalog(NativeLockMode::Shared);
        table(select->fromTable, NativeLockMode::Shared);
        for (const auto& join : select->joins) {
            table(join.table, NativeLockMode::Shared);
        }
    } else if (auto* insert = dynamic_cast<const InsertStmt*>(statement)) {
        catalog(NativeLockMode::Shared);
        table(insert->table, NativeLockMode::Exclusive);
    } else if (auto* update = dynamic_cast<const UpdateStmt*>(statement)) {
        catalog(NativeLockMode::Shared);
        table(update->table, NativeLockMode::Exclusive);
    } else if (auto* remove = dynamic_cast<const DeleteStmt*>(statement)) {
        catalog(NativeLockMode::Shared);
        table(remove->table, NativeLockMode::Exclusive);
    } else if (auto* create = dynamic_cast<const CreateStmt*>(statement)) {
        catalog(NativeLockMode::Exclusive);
        table(create->table, NativeLockMode::Exclusive);
    } else if (auto* drop = dynamic_cast<const DropStmt*>(statement)) {
        catalog(NativeLockMode::Exclusive);
        table(drop->table, NativeLockMode::Exclusive);
    } else if (auto* snapshot =
                   dynamic_cast<const CreateSnapshotStmt*>(statement)) {
        catalog(NativeLockMode::Exclusive);
        if (snapshot->source) {
            table(snapshot->source->fromTable, NativeLockMode::Shared);
        }
    } else if (dynamic_cast<const ExportTorchStmt*>(statement) ||
               dynamic_cast<const ExplainBatchStmt*>(statement) ||
               dynamic_cast<const SpillContextStmt*>(statement) ||
               dynamic_cast<const ExplainContextStmt*>(statement) ||
               dynamic_cast<const ShowTabsStmt*>(statement) ||
               dynamic_cast<const ShowContextSchemasStmt*>(statement) ||
               dynamic_cast<const ShowContextObjectsStmt*>(statement)) {
        catalog(NativeLockMode::Shared);
    } else if (dynamic_cast<const CreateContextStmt*>(statement) ||
               dynamic_cast<const AppendMemoryStmt*>(statement) ||
               dynamic_cast<const TagMemoryStmt*>(statement) ||
               dynamic_cast<const AliasTabStmt*>(statement) ||
               dynamic_cast<const MergeTabsStmt*>(statement)) {
        catalog(NativeLockMode::Exclusive);
    }

    return NativeLockManager::global().acquireAll(std::move(requests));
}

bool NativeEngine::statementMutatesPersistentState(
    const ASTNode* statement) const {
    return dynamic_cast<const CreateStmt*>(statement) ||
           dynamic_cast<const DropStmt*>(statement) ||
           dynamic_cast<const InsertStmt*>(statement) ||
           dynamic_cast<const UpdateStmt*>(statement) ||
           dynamic_cast<const DeleteStmt*>(statement) ||
           dynamic_cast<const CreateSnapshotStmt*>(statement) ||
           dynamic_cast<const CreateContextStmt*>(statement) ||
           dynamic_cast<const AppendMemoryStmt*>(statement) ||
           dynamic_cast<const TagMemoryStmt*>(statement) ||
           dynamic_cast<const AliasTabStmt*>(statement) ||
           dynamic_cast<const MergeTabsStmt*>(statement);
}

void NativeEngine::beginWalTransaction() {
    if (walTransactionId_ == 0) {
        walTransactionId_ = wal_.begin();
        walLoggedFiles_.clear();
    }
}

void NativeEngine::recordWalBefore(const std::filesystem::path& path) {
    if (walTransactionId_ == 0) return;
    const auto absolute =
        std::filesystem::absolute(path).lexically_normal();
    const auto key = absolute.generic_string();
    if (!walLoggedFiles_.insert(key).second) return;
    const auto before = std::filesystem::exists(absolute)
        ? std::optional<std::vector<std::uint8_t>>(readFile(absolute))
        : std::nullopt;
    wal_.logBefore(walTransactionId_, absolute, before);
}

void NativeEngine::commitWalTransaction() {
    if (walTransactionId_ == 0) return;
    wal_.commit(walTransactionId_);
    walTransactionId_ = 0;
    walLoggedFiles_.clear();
}

void NativeEngine::discardWalTransaction() {
    if (walTransactionId_ == 0) return;
    walTransactionId_ = 0;
    walLoggedFiles_.clear();
    bufferPool_.discardAll();
    mappedHeaps_.clear();
    wal_.recover();
    primaryIndexes_.clear();
    tableStatistics_.clear();
    reloadCatalog();
}

void NativeEngine::saveCatalog() {
    recordWalBefore(root_ / "catalog.json");
    catalog_.save();
}

void NativeEngine::flushUnlocked() {
    bufferPool_.flushAll();
    saveCatalog();
}

NativeQueryResult NativeEngine::executeCreate(
    const CreateStmt& statement) {
    if (catalog_.hasTable(statement.table)) {
        if (statement.ifNotExists) return {};
        throw std::runtime_error("Table already exists: " + statement.table);
    }

    TableMeta table;
    table.name = statement.table;
    std::unordered_set<std::string> columnNames;
    std::size_t primaryKeys = 0;
    for (const auto& definition : statement.columns) {
        if (!columnNames.insert(definition.name).second) {
            throw std::runtime_error(
                "Duplicate column: " + definition.name);
        }
        ColumnMeta column;
        column.name = definition.name;
        column.type = upper(definition.type);
        if (column.type != "INTEGER" && column.type != "INT" &&
            column.type != "REAL" && column.type != "FLOAT" &&
            column.type != "DOUBLE" && column.type != "TEXT" &&
            column.type != "VARCHAR" && column.type != "STRING" &&
            column.type != "BLOB") {
            throw std::runtime_error(
                "Unsupported column type: " + definition.type);
        }
        column.primary_key = definition.primary_key;
        if (column.primary_key && ++primaryKeys > 1) {
            throw std::runtime_error(
                "Only one primary key column is supported");
        }
        column.not_null = definition.not_null || definition.primary_key;
        column.fk_table = definition.fk_table;
        column.fk_col = definition.fk_col;
        if (!column.fk_table.empty()) {
            const auto* foreign = catalog_.getTable(column.fk_table);
            if (!foreign) {
                throw std::runtime_error(
                    "Foreign key table does not exist: " +
                    column.fk_table);
            }
            const bool targetExists = std::any_of(
                foreign->columns.begin(), foreign->columns.end(),
                [&](const ColumnMeta& target) {
                    return target.name == column.fk_col;
                });
            if (!targetExists) {
                throw std::runtime_error(
                    "Foreign key column does not exist: " +
                    column.fk_col);
            }
        }
        table.columns.push_back(std::move(column));
    }

    const auto path = tablePath(statement.table);
    recordWalBefore(path);
    auto file = heap(statement.table);
    file.create();
    try {
        catalog_.addTable(table);
        saveCatalog();
        primaryIndexes_.erase(statement.table);
        tableStatistics_.erase(statement.table);
        invalidateMappedHeap(statement.table);
    } catch (...) {
        file.drop();
        catalog_.removeTable(statement.table);
        throw;
    }
    NativeQueryResult result;
    result.message = "created table " + statement.table;
    return result;
}

NativeQueryResult NativeEngine::executeDrop(const DropStmt& statement) {
    const auto* existing = catalog_.getTable(statement.table);
    if (!existing) {
        if (statement.ifExists) return {};
        throw std::runtime_error("Unknown table: " + statement.table);
    }
    for (const auto& tableName : catalog_.tableNames()) {
        const auto* candidate = catalog_.getTable(tableName);
        if (!candidate || candidate->name == statement.table) continue;
        for (const auto& column : candidate->columns) {
            if (column.fk_table == statement.table) {
                throw std::runtime_error(
                    "Cannot drop referenced table " + statement.table);
            }
        }
    }
    const TableMeta backup = *existing;
    const auto path = tablePath(statement.table);
    bufferPool_.flushFile(path);
    std::optional<std::vector<std::uint8_t>> bytes;
    if (std::filesystem::exists(path)) bytes = readFile(path);
    recordWalBefore(path);

    try {
        invalidateMappedHeap(statement.table);
        heap(statement.table).drop();
        primaryIndexes_.erase(statement.table);
        tableStatistics_.erase(statement.table);
        catalog_.removeTable(statement.table);
        saveCatalog();
    } catch (...) {
        catalog_.addTable(backup);
        restoreFile(path, bytes);
        saveCatalog();
        throw;
    }
    NativeQueryResult result;
    result.message = "dropped table " + statement.table;
    return result;
}

NativeQueryResult NativeEngine::executeInsert(
    const InsertStmt& statement) {
    const auto* table = catalog_.getTable(statement.table);
    if (!table) throw std::runtime_error("Unknown table: " + statement.table);

    auto file = heap(statement.table);
    invalidateMappedHeap(statement.table);
    file.flush();
    const auto path = tablePath(statement.table);
    const auto before = std::filesystem::exists(path)
        ? std::optional<std::vector<std::uint8_t>>(readFile(path))
        : std::nullopt;
    recordWalBefore(path);

    NativeQueryResult result;
    try {
        for (const auto& valueRow : statement.valueRows) {
            Tuple tuple(table->columns.size(), Value::null());
            if (statement.columns.empty()) {
                if (valueRow.size() != tuple.size()) {
                    throw std::runtime_error(
                        "INSERT value count does not match table schema");
                }
                for (std::size_t index = 0; index < valueRow.size(); ++index) {
                    tuple[index] = eval(valueRow[index].get(), EvalRow{});
                }
            } else {
                if (valueRow.size() != statement.columns.size()) {
                    throw std::runtime_error(
                        "INSERT value count does not match column list");
                }
                for (std::size_t index = 0;
                     index < statement.columns.size();
                     ++index) {
                    auto found = std::find_if(
                        table->columns.begin(), table->columns.end(),
                        [&](const ColumnMeta& column) {
                            return column.name == statement.columns[index];
                        });
                    if (found == table->columns.end()) {
                        throw std::runtime_error(
                            "Unknown column: " + statement.columns[index]);
                    }
                    const auto position = static_cast<std::size_t>(
                        std::distance(table->columns.begin(), found));
                    tuple[position] =
                        eval(valueRow[index].get(), EvalRow{});
                }
            }
            tuple = validateAndCoerce(*table, std::move(tuple), std::nullopt);
            const RowId rowId = file.insert(tuple);
            auto primary = std::find_if(
                table->columns.begin(), table->columns.end(),
                [](const ColumnMeta& column) {
                    return column.primary_key;
                });
            if (primary != table->columns.end()) {
                const auto position = static_cast<std::size_t>(
                    std::distance(table->columns.begin(), primary));
                primaryIndex(table->name).insert(tuple[position], rowId);
            }
            ++result.rowsAffected;
            ++stats_.rowsWritten;
        }
        file.flush();
        tableStatistics_.erase(statement.table);
        invalidateMappedHeap(statement.table);
    } catch (...) {
        primaryIndexes_.erase(statement.table);
        bufferPool_.invalidateFile(path);
        restoreFile(path, before);
        throw;
    }
    result.message = "inserted " + std::to_string(result.rowsAffected) +
                     " row(s)";
    return result;
}

NativeQueryResult NativeEngine::executeUpdate(
    const UpdateStmt& statement) {
    const auto* table = catalog_.getTable(statement.table);
    if (!table) throw std::runtime_error("Unknown table: " + statement.table);
    auto file = heap(statement.table);
    invalidateMappedHeap(statement.table);
    file.flush();
    const auto path = tablePath(statement.table);
    const auto before = readFile(path);
    recordWalBefore(path);

    NativeQueryResult result;
    try {
        auto rows = scanTable(statement.table, statement.table);
        for (auto& scanned : rows) {
            if (statement.where &&
                !eval(statement.where.get(), scanned.row).asBoolean()) {
                continue;
            }
            Tuple updated = scanned.row.values;
            for (const auto& assignment : statement.sets) {
                auto found = std::find_if(
                    table->columns.begin(), table->columns.end(),
                    [&](const ColumnMeta& column) {
                        return column.name == assignment.column;
                    });
                if (found == table->columns.end()) {
                    throw std::runtime_error(
                        "Unknown column: " + assignment.column);
                }
                const auto position = static_cast<std::size_t>(
                    std::distance(table->columns.begin(), found));
                updated[position] =
                    eval(assignment.value.get(), scanned.row);
            }
            updated = validateAndCoerce(
                *table, std::move(updated), scanned.id);
            validateNoIncomingReferences(
                *table, scanned.row.values, &updated);
            file.update(scanned.id, updated);
            ++result.rowsAffected;
            ++stats_.rowsWritten;
        }
        file.flush();
        primaryIndexes_.erase(statement.table);
        tableStatistics_.erase(statement.table);
        invalidateMappedHeap(statement.table);
    } catch (...) {
        bufferPool_.invalidateFile(path);
        restoreFile(path, before);
        throw;
    }
    result.message = "updated " + std::to_string(result.rowsAffected) +
                     " row(s)";
    return result;
}

NativeQueryResult NativeEngine::executeDelete(
    const DeleteStmt& statement) {
    if (!catalog_.hasTable(statement.table)) {
        throw std::runtime_error("Unknown table: " + statement.table);
    }
    auto file = heap(statement.table);
    invalidateMappedHeap(statement.table);
    file.flush();
    const auto path = tablePath(statement.table);
    const auto before = readFile(path);
    recordWalBefore(path);

    NativeQueryResult result;
    try {
        auto rows = scanTable(statement.table, statement.table);
        for (const auto& scanned : rows) {
            if (statement.where &&
                !eval(statement.where.get(), scanned.row).asBoolean()) {
                continue;
            }
            validateNoIncomingReferences(
                *catalog_.getTable(statement.table),
                scanned.row.values);
            if (file.erase(scanned.id)) {
                ++result.rowsAffected;
                ++stats_.rowsWritten;
            }
        }
        file.flush();
        primaryIndexes_.erase(statement.table);
        tableStatistics_.erase(statement.table);
        invalidateMappedHeap(statement.table);
    } catch (...) {
        bufferPool_.invalidateFile(path);
        restoreFile(path, before);
        throw;
    }
    result.message = "deleted " + std::to_string(result.rowsAffected) +
                     " row(s)";
    return result;
}

NativeQueryResult NativeEngine::executeCreateSnapshot(
    const CreateSnapshotStmt& statement) {
    if (catalog_.hasSnapshot(statement.name)) {
        throw std::runtime_error(
            "Snapshot already exists: " + statement.name);
    }
    if (!statement.source) {
        throw std::runtime_error("Snapshot requires a source query");
    }
    const auto& source = *statement.source;
    if (!source.joins.empty() || !source.groupBy.empty() ||
        source.having || source.limit || source.offset) {
        throw std::runtime_error(
            "manifest-snapshot currently supports one table with an optional filter");
    }

    const auto* table = catalog_.getTable(source.fromTable);
    if (!table) {
        throw std::runtime_error("Unknown source table: " + source.fromTable);
    }

    std::vector<std::string> selectedColumns;
    bool selectedWildcard = false;
    for (const auto& expression : source.columns) {
        if (dynamic_cast<const Wildcard*>(expression.get())) {
            selectedWildcard = true;
            continue;
        }
        auto* column = dynamic_cast<const ColumnRef*>(expression.get());
        if (!column) {
            throw std::runtime_error(
                "manifest-snapshot features must be projected columns");
        }
        selectedColumns.push_back(column->column);
    }

    DatasetSnapshotMeta snapshot;
    snapshot.name = statement.name;
    snapshot.sourceTable = source.fromTable;
    snapshot.schemaFingerprint =
        std::to_string(catalog_.schemaFingerprint());
    snapshot.tableVersion = std::to_string(catalog_.revision());
    snapshot.splitBy = statement.splitBy.empty() ? "row" : statement.splitBy;
    snapshot.seed = statement.seed;
    snapshot.label.name = statement.label.name;
    snapshot.label.spec = statement.label.spec;

    for (const auto& feature : statement.features) {
        snapshot.features.push_back({feature.name, feature.spec});
    }

    if (snapshot.label.name.empty()) {
        if (!selectedColumns.empty()) {
            snapshot.label.name = selectedColumns.back();
        } else if (selectedWildcard && !table->columns.empty()) {
            snapshot.label.name = table->columns.back().name;
        }
    }

    if (snapshot.features.empty()) {
        if (selectedWildcard) {
            for (const auto& column : table->columns) {
                if (column.name != snapshot.label.name) {
                    snapshot.features.push_back({column.name, ""});
                }
            }
        } else {
            for (const auto& column : selectedColumns) {
                if (column != snapshot.label.name) {
                    snapshot.features.push_back({column, ""});
                }
            }
        }
    }

    if (snapshot.features.empty()) {
        throw std::runtime_error(
            "manifest-snapshot requires at least one feature");
    }
    if (snapshot.label.name.empty()) {
        throw std::runtime_error("manifest-snapshot requires a label");
    }

    for (const auto& feature : snapshot.features) {
        if (!tableColumnIndex(*table, feature.name)) {
            throw std::runtime_error(
                "Unknown feature column: " + feature.name);
        }
    }
    const auto labelIndex = tableColumnIndex(*table, snapshot.label.name);
    if (!labelIndex) {
        throw std::runtime_error(
            "Unknown label column: " + snapshot.label.name);
    }

    const bool splitByRow =
        snapshot.splitBy == "row" || snapshot.splitBy == "rowid";
    if (!splitByRow && !tableColumnIndex(*table, snapshot.splitBy)) {
        throw std::runtime_error(
            "Unknown split key column: " + snapshot.splitBy);
    }

    CodeGen codegen;
    snapshot.queryText = codegen.generate(&source);

    std::unordered_map<std::string, std::set<std::string>> splitKeySplits;
    std::unordered_map<std::string, std::set<std::string>> userIdSplits;
    const auto userIdIndex = tableColumnIndex(*table, "user_id");

    auto rows = scanTable(source.fromTable, source.fromAlias);
    for (const auto& scanned : rows) {
        if (source.where &&
            !eval(source.where.get(), scanned.row).asBoolean()) {
            continue;
        }

        const std::string rowid = rowIdString(scanned.id);
        std::string splitKey = rowid;
        if (!splitByRow) {
            ColumnRef splitColumn;
            splitColumn.column = snapshot.splitBy;
            splitKey = resolveColumn(splitColumn, scanned.row).toString();
        }
        const std::string split = splitForKey(snapshot.seed, splitKey);
        snapshot.rows.push_back({rowid, split});

        if (!splitByRow) {
            splitKeySplits[splitKey].insert(split);
        }
        if (userIdIndex && snapshot.splitBy != "user_id" &&
            *userIdIndex < scanned.row.values.size()) {
            userIdSplits[scanned.row.values[*userIdIndex].toString()]
                .insert(split);
        }
    }

    for (const auto& entry : splitKeySplits) {
        if (entry.second.size() > 1) {
            snapshot.warnings.push_back(
                "leakage-check sus: split key '" + entry.first +
                "' appears in multiple splits");
            break;
        }
    }
    for (const auto& entry : userIdSplits) {
        if (entry.second.size() > 1) {
            snapshot.warnings.push_back(
                "leakage-check sus: user_id '" + entry.first +
                "' appears in multiple splits; prefer split-by user_id");
            break;
        }
    }

    catalog_.addSnapshot(snapshot);
    saveCatalog();

    std::size_t train = 0;
    std::size_t validation = 0;
    std::size_t test = 0;
    for (const auto& row : snapshot.rows) {
        if (row.split == "train") ++train;
        else if (row.split == "validation") ++validation;
        else if (row.split == "test") ++test;
    }

    std::vector<std::string> featureNames;
    featureNames.reserve(snapshot.features.size());
    for (const auto& feature : snapshot.features) {
        featureNames.push_back(feature.name);
    }

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "snapshot", snapshot.name);
    addKeyValue(result, "rows", std::to_string(snapshot.rows.size()));
    addKeyValue(result, "train_rows", std::to_string(train));
    addKeyValue(result, "validation_rows", std::to_string(validation));
    addKeyValue(result, "test_rows", std::to_string(test));
    addKeyValue(result, "split_by", snapshot.splitBy);
    addKeyValue(result, "seed", std::to_string(snapshot.seed));
    addKeyValue(result, "features", joinStrings(featureNames, ","));
    addKeyValue(result, "label", snapshot.label.name);
    if (!snapshot.warnings.empty()) {
        addKeyValue(result, "warning",
                    joinStrings(snapshot.warnings, " | "));
    }
    result.message = "manifested snapshot " + snapshot.name;
    return result;
}

NativeQueryResult NativeEngine::executeExportTorch(
    const ExportTorchStmt& statement) {
    const auto* snapshot = catalog_.getSnapshot(statement.dataset);
    if (!snapshot) {
        throw std::runtime_error("Unknown snapshot: " + statement.dataset);
    }
    if (statement.batchSize == 0) {
        throw std::runtime_error("batch-size must be greater than zero");
    }
    const auto planned = plannedSnapshotRows(
        *snapshot, statement.epoch, statement.rank, statement.worldSize);
    const auto batches =
        (planned.size() + static_cast<std::size_t>(statement.batchSize) - 1) /
        static_cast<std::size_t>(statement.batchSize);

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "snapshot", snapshot->name);
    addKeyValue(result, "torch_dataset",
                "from tensorql import TensorQLDataset");
    addKeyValue(result, "python",
                "TensorQLDataset(db_path, dataset=\"" + snapshot->name +
                "\", batch_size=" +
                std::to_string(statement.batchSize) + ")");
    addKeyValue(result, "samples", std::to_string(planned.size()));
    addKeyValue(result, "batches", std::to_string(batches));
    addKeyValue(result, "batch_size", std::to_string(statement.batchSize));
    addKeyValue(result, "shuffle", statement.deterministicShuffle
                ? "deterministic" : "off");
    addKeyValue(result, "epoch", std::to_string(statement.epoch));
    addKeyValue(result, "rank", std::to_string(statement.rank));
    addKeyValue(result, "world_size", std::to_string(statement.worldSize));
    result.message = "torch export plan ready for " + snapshot->name;
    return result;
}

NativeQueryResult NativeEngine::executeExplainBatch(
    const ExplainBatchStmt& statement) {
    const auto* snapshot = catalog_.getSnapshot(statement.dataset);
    if (!snapshot) {
        throw std::runtime_error("Unknown snapshot: " + statement.dataset);
    }
    if (statement.batchSize == 0) {
        throw std::runtime_error("batch-size must be greater than zero");
    }

    const auto planned = plannedSnapshotRows(
        *snapshot, statement.epoch, statement.rank, statement.worldSize);
    const auto start =
        static_cast<std::size_t>(statement.batch) *
        static_cast<std::size_t>(statement.batchSize);
    const auto end = std::min(
        planned.size(),
        start + static_cast<std::size_t>(statement.batchSize));

    std::vector<const SnapshotRowMeta*> batchRows;
    if (start < planned.size()) {
        batchRows.insert(batchRows.end(),
                         planned.begin() + static_cast<std::ptrdiff_t>(start),
                         planned.begin() + static_cast<std::ptrdiff_t>(end));
    }

    const auto* table = catalog_.getTable(snapshot->sourceTable);
    if (!table) {
        throw std::runtime_error(
            "Snapshot source table disappeared: " + snapshot->sourceTable);
    }
    const auto labelIndex = tableColumnIndex(*table, snapshot->label.name);
    if (!labelIndex) {
        throw std::runtime_error(
            "Snapshot label column disappeared: " + snapshot->label.name);
    }

    std::map<std::string, std::size_t> labelCounts;
    auto file = heap(snapshot->sourceTable);
    std::vector<std::string> rowids;
    rowids.reserve(batchRows.size());
    for (const auto* row : batchRows) {
        rowids.push_back(row->rowid);
        auto stored = file.read(parseRowIdString(row->rowid));
        if (!stored || *labelIndex >= stored->values.size()) continue;
        ++labelCounts[stored->values[*labelIndex].toString()];
    }

    std::vector<std::string> labelParts;
    for (const auto& entry : labelCounts) {
        labelParts.push_back(entry.first + "=" +
                             std::to_string(entry.second));
    }
    std::vector<std::string> featureNames;
    for (const auto& feature : snapshot->features) {
        featureNames.push_back(feature.name);
    }

    NativeQueryResult result;
    result.columns = {"field", "value"};
    addKeyValue(result, "snapshot", snapshot->name);
    addKeyValue(result, "batch", std::to_string(statement.batch));
    addKeyValue(result, "samples", std::to_string(batchRows.size()));
    addKeyValue(result, "source_rows",
                rowids.empty() ? "" : joinStrings(rowids, ","));
    if (!rowids.empty()) {
        addKeyValue(result, "source_range",
                    rowids.front() + "-" + rowids.back());
    }
    addKeyValue(result, "feature_columns",
                joinStrings(featureNames, ","));
    addKeyValue(result, "label_distribution",
                joinStrings(labelParts, ","));
    addKeyValue(result, "split", "train");
    addKeyValue(result, "seed", std::to_string(snapshot->seed));
    addKeyValue(result, "epoch", std::to_string(statement.epoch));
    addKeyValue(result, "rank",
                std::to_string(statement.rank) + "/" +
                std::to_string(statement.worldSize));
    addKeyValue(result, "worker", "0");
    addKeyValue(result, "resume_token",
                snapshot->name + ":epoch=" +
                std::to_string(statement.epoch) + ":batch=" +
                std::to_string(statement.batch) + ":rank=" +
                std::to_string(statement.rank));
    result.message = "spilled batch provenance for " + snapshot->name;
    return result;
}

NativeQueryResult NativeEngine::executeSelect(
    const SelectStmt& statement) {
    if (auto rawPoint = executeRawPointSelect(statement)) {
        return std::move(*rawPoint);
    }

    if (!primaryKeyPredicate(statement.fromTable,
                             statement.fromAlias,
                             statement.where.get())) {
        if (auto direct = executeDirectAggregateSelect(statement)) {
            return std::move(*direct);
        }
        if (auto vectorized = executeVectorizedSelect(statement)) {
            return std::move(*vectorized);
        }
        if (auto rowIdSeek =
                executeRowIdSeekJoinAggregateSelect(statement)) {
            return std::move(*rowIdSeek);
        }
    }

    std::vector<EvalRow> rows;
    auto plannedRows = executeCostBasedJoins(statement);
    if (plannedRows) {
        rows = std::move(*plannedRows);
    } else {
        const auto pointKey = primaryKeyPredicate(
            statement.fromTable, statement.fromAlias,
            statement.where.get());
        auto base = pointKey
            ? lookupPrimaryKey(statement.fromTable,
                               statement.fromAlias,
                               *pointKey)
            : scanTable(statement.fromTable, statement.fromAlias);
        rows.reserve(base.size());
        for (auto& item : base) rows.push_back(std::move(item.row));
    }

    if (!plannedRows) {
        executeSourceOrderJoins(statement, rows);
    }

    if (statement.where) {
        rows.erase(std::remove_if(
            rows.begin(), rows.end(),
            [&](const EvalRow& row) {
                return !eval(statement.where.get(), row).asBoolean();
            }), rows.end());
    }

    bool wholeRowProjection = false;
    for (const auto& expression : statement.columns) {
        auto* function =
            dynamic_cast<const FunctionCall*>(expression.get());
        if (!function ||
            (function->name != "biggest-W" &&
             function->name != "biggest-L")) {
            continue;
        }
        if (function->args.empty()) break;
        const bool descending = function->name == "biggest-W";
        std::stable_sort(rows.begin(), rows.end(),
            [&](const EvalRow& left, const EvalRow& right) {
                const int comparison =
                    eval(function->args.front().get(), left).compare(
                        eval(function->args.front().get(), right));
                return descending ? comparison > 0 : comparison < 0;
            });
        if (rows.size() > 1) rows.resize(1);
        wholeRowProjection = true;
        break;
    }

    const EvalRow* sample = rows.empty() ? nullptr : &rows.front();
    NativeQueryResult result;
    if (wholeRowProjection && sample) {
        for (const auto& column : *sample->columns) {
            result.columns.push_back(column.name);
        }
    } else {
        result.columns = outputNames(statement, sample);
    }
    std::vector<OutputRow> output;

    const bool grouped = !statement.groupBy.empty() ||
                         selectHasAggregate(statement);
    if (grouped) {
        auto tryStreamingAggregation =
            [&]() -> std::optional<std::vector<OutputRow>> {
            if (statement.having || statement.distinct ||
                !statement.orderBy.empty() || statement.limit ||
                statement.offset) {
                return std::nullopt;
            }

            enum class StreamAggregateKind {
                Count, Sum, Average, Max, Min, LoneWolf
            };
            struct StreamProjection {
                bool aggregate = false;
                const ASTNode* expression = nullptr;
                StreamAggregateKind kind = StreamAggregateKind::Count;
                const ASTNode* argument = nullptr;
                bool countWildcard = false;
            };
            struct StreamAggregateState {
                std::int64_t count = 0;
                double sum = 0.0;
                bool allIntegers = true;
                std::optional<Value> extremum;
                std::vector<double> samples;
            };
            struct StreamGroupState {
                Tuple representatives;
                std::vector<StreamAggregateState> aggregates;
            };

            std::vector<StreamProjection> projections;
            projections.reserve(statement.columns.size());
            for (const auto& column : statement.columns) {
                const auto* function =
                    dynamic_cast<const FunctionCall*>(column.get());
                if (!function) {
                    if (hasAggregate(column.get())) return std::nullopt;
                    projections.push_back(
                        StreamProjection{false, column.get()});
                    continue;
                }
                if (function->distinct ||
                    !isSimpleAggregateName(function->name)) {
                    return std::nullopt;
                }
                StreamProjection projection;
                projection.aggregate = true;
                projection.expression = column.get();
                if (function->name == "headcount") {
                    projection.kind = StreamAggregateKind::Count;
                    projection.countWildcard =
                        function->args.empty() ||
                        dynamic_cast<const Wildcard*>(
                            function->args.front().get());
                } else if (function->name == "stack") {
                    projection.kind = StreamAggregateKind::Sum;
                } else if (function->name == "mid") {
                    projection.kind = StreamAggregateKind::Average;
                } else if (function->name == "goat") {
                    projection.kind = StreamAggregateKind::Max;
                } else if (function->name == "L") {
                    projection.kind = StreamAggregateKind::Min;
                } else if (isLoneWolfName(function->name)) {
                    projection.kind = StreamAggregateKind::LoneWolf;
                }
                if (!projection.countWildcard) {
                    if (function->args.size() != 1) return std::nullopt;
                    projection.argument = function->args.front().get();
                }
                projections.push_back(projection);
            }

            std::unordered_map<Tuple, StreamGroupState,
                               TupleKeyHash, TupleKeyEqual> groups;
            auto initialize = [&](StreamGroupState& group) {
                if (!group.representatives.empty() ||
                    !group.aggregates.empty()) {
                    return;
                }
                group.representatives.resize(
                    projections.size(), Value::null());
                group.aggregates.resize(projections.size());
            };
            if (statement.groupBy.empty()) {
                initialize(groups[{}]);
            }

            for (const auto& row : rows) {
                Tuple key;
                key.reserve(statement.groupBy.size());
                for (const auto& expression : statement.groupBy) {
                    key.push_back(eval(expression.get(), row));
                }
                auto& group = groups[key];
                initialize(group);
                for (std::size_t index = 0;
                     index < projections.size();
                     ++index) {
                    const auto& projection = projections[index];
                    if (!projection.aggregate) {
                        if (group.representatives[index].isNull()) {
                            group.representatives[index] =
                                eval(projection.expression, row);
                        }
                        continue;
                    }

                    auto& state = group.aggregates[index];
                    if (projection.countWildcard) {
                        ++state.count;
                        continue;
                    }
                    Value value = eval(projection.argument, row);
                    if (value.isNull()) continue;
                    if (projection.kind == StreamAggregateKind::Count) {
                        ++state.count;
                        continue;
                    }
                    if (projection.kind == StreamAggregateKind::Sum ||
                        projection.kind == StreamAggregateKind::Average ||
                        projection.kind == StreamAggregateKind::LoneWolf) {
                        if (!value.isNumeric()) {
                            throw std::runtime_error(
                                "Numeric aggregate on non-number");
                        }
                        ++state.count;
                        const double numeric = value.asReal();
                        state.sum += numeric;
                        state.allIntegers =
                            state.allIntegers &&
                            value.type() == ValueType::Integer;
                        if (projection.kind ==
                            StreamAggregateKind::LoneWolf) {
                            state.samples.push_back(numeric);
                        }
                        continue;
                    }
                    ++state.count;
                    if (!state.extremum) {
                        state.extremum = std::move(value);
                        continue;
                    }
                    const int comparison =
                        value.compare(*state.extremum);
                    if ((projection.kind == StreamAggregateKind::Max &&
                         comparison > 0) ||
                        (projection.kind == StreamAggregateKind::Min &&
                         comparison < 0)) {
                        state.extremum = std::move(value);
                    }
                }
            }

            std::vector<OutputRow> streamed;
            streamed.reserve(groups.size());
            for (auto& item : groups) {
                initialize(item.second);
                OutputRow row;
                row.values.reserve(projections.size());
                for (std::size_t index = 0;
                     index < projections.size();
                     ++index) {
                    const auto& projection = projections[index];
                    if (!projection.aggregate) {
                        row.values.push_back(
                            item.second.representatives[index]);
                        continue;
                    }
                    const auto& state = item.second.aggregates[index];
                    if (projection.kind == StreamAggregateKind::Count) {
                        row.values.push_back(Value(state.count));
                    } else if (projection.kind ==
                               StreamAggregateKind::LoneWolf) {
                        row.values.push_back(
                            Value(countLoneWolves(state.samples)));
                    } else if (state.count == 0) {
                        row.values.push_back(Value::null());
                    } else if (projection.kind ==
                               StreamAggregateKind::Average) {
                        row.values.push_back(Value(
                            state.sum /
                            static_cast<double>(state.count)));
                    } else if (projection.kind ==
                               StreamAggregateKind::Sum) {
                        row.values.push_back(state.allIntegers
                            ? Value(static_cast<std::int64_t>(state.sum))
                            : Value(state.sum));
                    } else {
                        row.values.push_back(
                            state.extremum.value_or(Value::null()));
                    }
                }
                streamed.push_back(std::move(row));
            }
            ++stats_.streamingAggregateQueries;
            stats_.streamingAggregateRows += rows.size();
            return streamed;
        };

        if (auto streamed = tryStreamingAggregation()) {
            output = std::move(*streamed);
        } else {
        std::unordered_map<Tuple, std::vector<EvalRow>,
                           TupleKeyHash, TupleKeyEqual> groups;
        if (statement.groupBy.empty()) {
            groups[{}] = rows;
        } else {
            for (const auto& row : rows) {
                Tuple key;
                for (const auto& expression : statement.groupBy) {
                    key.push_back(eval(expression.get(), row));
                }
                groups[key].push_back(row);
            }
        }

        for (auto& item : groups) {
            auto& groupRows = item.second;
            if (statement.having &&
                !evalGrouped(statement.having.get(),
                             groupRows).asBoolean()) {
                continue;
            }
            OutputRow row;
            row.values = projectGrouped(statement, groupRows);
            row.orderKeys = orderKeysGrouped(
                statement, groupRows, result.columns, row.values);
            output.push_back(std::move(row));
        }
        }
    } else {
        for (const auto& source : rows) {
            OutputRow row;
            row.values = wholeRowProjection
                ? source.values
                : project(statement, source, &rows);
            row.orderKeys = orderKeys(
                statement, source, result.columns, row.values);
            output.push_back(std::move(row));
        }
    }

    if (statement.distinct) {
        std::unordered_set<Tuple, TupleKeyHash, TupleKeyEqual> seen;
        output.erase(std::remove_if(
            output.begin(), output.end(),
            [&](const OutputRow& row) {
                return !seen.insert(row.values).second;
            }), output.end());
    }

    if (!statement.orderBy.empty()) {
        std::stable_sort(output.begin(), output.end(),
            [&](const OutputRow& left, const OutputRow& right) {
                for (std::size_t index = 0;
                     index < statement.orderBy.size();
                     ++index) {
                    const int comparison =
                        left.orderKeys[index].compare(right.orderKeys[index]);
                    if (comparison == 0) continue;
                    return statement.orderBy[index].asc
                        ? comparison < 0 : comparison > 0;
                }
                return false;
            });
    }

    const std::size_t offset = static_cast<std::size_t>(
        std::max<std::int64_t>(0,
            integerLiteral(statement.offset.get(), 0)));
    const std::size_t limit = statement.limit
        ? static_cast<std::size_t>(std::max<std::int64_t>(
              0, integerLiteral(statement.limit.get(), 0)))
        : std::numeric_limits<std::size_t>::max();
    for (std::size_t index = offset;
         index < output.size() && result.rows.size() < limit;
         ++index) {
        result.rows.push_back(std::move(output[index].values));
    }
    return result;
}

std::filesystem::path NativeEngine::tablePath(
    const std::string& table) const {
    for (unsigned char ch : table) {
        if (!std::isalnum(ch) && ch != '_') {
            throw std::runtime_error("Unsafe table name: " + table);
        }
    }
    return root_ / "tables" / (table + ".heap");
}

HeapFile NativeEngine::heap(const std::string& table) {
    return HeapFile(tablePath(table), bufferPool_);
}

std::shared_ptr<const std::vector<NativeEngine::BoundColumn>>
NativeEngine::boundSchema(const std::string& table,
                          const std::string& alias) const {
    const auto* metadata = catalog_.getTable(table);
    if (!metadata) throw std::runtime_error("Unknown table: " + table);
    auto columns = std::make_shared<std::vector<BoundColumn>>();
    columns->reserve(metadata->columns.size());
    for (const auto& column : metadata->columns) {
        columns->push_back({table, alias, column.name});
    }
    return columns;
}

std::shared_ptr<const std::vector<NativeEngine::BoundColumn>>
NativeEngine::combineSchemas(
    const std::shared_ptr<const std::vector<BoundColumn>>& left,
    const std::shared_ptr<const std::vector<BoundColumn>>& right) {
    auto columns = std::make_shared<std::vector<BoundColumn>>();
    columns->reserve((left ? left->size() : 0) +
                     (right ? right->size() : 0));
    if (left) columns->insert(
        columns->end(), left->begin(), left->end());
    if (right) columns->insert(
        columns->end(), right->begin(), right->end());
    return columns;
}

std::vector<NativeEngine::ScanRow> NativeEngine::scanTable(
    const std::string& table,
    const std::string& alias) {
    const auto* metadata = catalog_.getTable(table);
    if (!metadata) throw std::runtime_error("Unknown table: " + table);
    auto stored = heap(table).scan();
    ++stats_.tableScans;
    stats_.rowsRead += stored.size();

    std::vector<ScanRow> result;
    result.reserve(stored.size());
    const auto columns = boundSchema(table, alias);
    for (auto& row : stored) {
        if (row.values.size() != metadata->columns.size()) {
            throw std::runtime_error("Tuple/schema width mismatch in " + table);
        }
        EvalRow bound;
        bound.values = std::move(row.values);
        bound.columns = columns;
        result.push_back({row.id, std::move(bound)});
    }
    return result;
}

std::vector<NativeEngine::ScanRow> NativeEngine::lookupPrimaryKey(
    const std::string& table,
    const std::string& alias,
    const Value& key) {
    const auto* metadata = catalog_.getTable(table);
    if (!metadata) throw std::runtime_error("Unknown table: " + table);
    const auto rowId = primaryIndex(table).find(key);
    ++stats_.indexLookups;
    if (!rowId) return {};
    auto stored = heap(table).read(*rowId);
    if (!stored) {
        primaryIndexes_.erase(table);
        return lookupPrimaryKey(table, alias, key);
    }
    EvalRow bound;
    bound.values = std::move(stored->values);
    bound.columns = boundSchema(table, alias);
    ++stats_.rowsRead;
    return {ScanRow{stored->id, std::move(bound)}};
}

BPlusTree& NativeEngine::primaryIndex(const std::string& table) {
    auto existing = primaryIndexes_.find(table);
    if (existing != primaryIndexes_.end()) return *existing->second;

    const auto* metadata = catalog_.getTable(table);
    if (!metadata) throw std::runtime_error("Unknown table: " + table);
    auto primary = std::find_if(
        metadata->columns.begin(), metadata->columns.end(),
        [](const ColumnMeta& column) { return column.primary_key; });
    if (primary == metadata->columns.end()) {
        throw std::runtime_error("Table has no primary key index: " + table);
    }
    const auto position = static_cast<std::size_t>(
        std::distance(metadata->columns.begin(), primary));
    auto index = std::make_unique<BPlusTree>();
    for (const auto& row : heap(table).scan()) {
        index->insert(row.values[position], row.id);
    }
    auto* pointer = index.get();
    primaryIndexes_[table] = std::move(index);
    return *pointer;
}

MappedHeapFile& NativeEngine::mappedHeap(const std::string& table) {
    auto existing = mappedHeaps_.find(table);
    if (existing != mappedHeaps_.end()) return existing->second;
    auto mapped = heap(table).mappedView();
    auto inserted = mappedHeaps_.emplace(table, std::move(mapped));
    return inserted.first->second;
}

void NativeEngine::invalidateMappedHeap(const std::string& table) {
    mappedHeaps_.erase(table);
}

const NativeEngine::TableStatistics::ColumnRange&
NativeEngine::ensureColumnRange(const std::string& table,
                                const std::string& columnName) {
    const auto* metadata = catalog_.getTable(table);
    if (!metadata) throw std::runtime_error("Unknown table: " + table);
    auto cachedRange = tableStatistics_[table].columnRanges.find(
        columnName);
    if (cachedRange != tableStatistics_[table].columnRanges.end()) {
        return cachedRange->second;
    }

    auto foundColumn = std::find_if(
        metadata->columns.begin(), metadata->columns.end(),
        [&](const ColumnMeta& column) {
            return column.name == columnName;
        });
    if (foundColumn == metadata->columns.end()) {
        throw std::runtime_error(
            "Unknown statistics column: " + columnName);
    }
    const auto column = static_cast<std::size_t>(
        std::distance(metadata->columns.begin(), foundColumn));

    TableStatistics::ColumnRange range;
    std::size_t rows = 0;
    std::vector<double> numericSamples;
    bool valueCountsExact = true;
    const std::size_t maxExactValueCounts =
        skibidi::config::exactValueCountLimit();
    auto file = heap(table);
    file.scanRawRowsFast(
        [&](RowId, const std::uint8_t* data, std::size_t length) {
        ++rows;
        RawField field = decodeRawColumn(data, length, column);
        if (field.isNull) return;
        Value value = field.toValue();
        if (valueCountsExact) {
            ++range.valueCounts[value];
            if (range.valueCounts.size() > maxExactValueCounts) {
                range.valueCounts.clear();
                valueCountsExact = false;
            }
        }
        if (!range.present) {
            range.present = true;
            range.min = value;
            range.max = value;
        } else {
            if (value.compare(range.min) < 0) range.min = value;
            if (value.compare(range.max) > 0) range.max = value;
        }
        ++range.nonNullCount;
        if (field.numeric()) {
            const double numeric = field.asReal();
            range.numeric = true;
            double power = numeric;
            for (double& moment : range.rawMoments) {
                moment += power;
                power *= numeric;
            }
            numericSamples.push_back(numeric);
        }
    });
    range.valueCountsExact = valueCountsExact;

    if (range.numeric && !numericSamples.empty()) {
        double minimum = numericSamples.front();
        double maximum = numericSamples.front();
        for (const double value : numericSamples) {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        range.bucketMin = minimum;
        range.bucketMax = maximum;
        if (minimum == maximum) {
            range.buckets[0] = numericSamples.size();
        } else {
            const double width = maximum - minimum;
            for (const double value : numericSamples) {
                std::size_t bucket = static_cast<std::size_t>(
                    ((value - minimum) / width) *
                    static_cast<double>(range.buckets.size()));
                if (bucket >= range.buckets.size()) {
                    bucket = range.buckets.size() - 1;
                }
                ++range.buckets[bucket];
            }
        }
    }

    auto& cached = tableStatistics_[table];
    if (cached.rowCount != 0 && cached.rowCount != rows) {
        cached = {};
    }
    cached.rowCount = rows;
    ++stats_.minMaxStatisticsBuilt;
    stats_.minMaxBuildRows += rows;
    auto inserted = cached.columnRanges.emplace(columnName, std::move(range));
    return inserted.first->second;
}

std::optional<Value> NativeEngine::primaryKeyPredicate(
    const std::string& table,
    const std::string& alias,
    const ASTNode* expression) const {
    const auto* metadata = catalog_.getTable(table);
    if (!metadata || !expression) return std::nullopt;
    auto primary = std::find_if(
        metadata->columns.begin(), metadata->columns.end(),
        [](const ColumnMeta& column) { return column.primary_key; });
    if (primary == metadata->columns.end()) return std::nullopt;

    auto* binary = dynamic_cast<const BinaryOp*>(expression);
    if (!binary) return std::nullopt;
    if (binary->op == "AND") {
        auto left = primaryKeyPredicate(
            table, alias, binary->left.get());
        if (left) return left;
        return primaryKeyPredicate(table, alias, binary->right.get());
    }
    if (binary->op != "=") return std::nullopt;

    auto match = [&](const ASTNode* columnNode,
                     const ASTNode* valueNode) -> std::optional<Value> {
        auto* column = dynamic_cast<const ColumnRef*>(columnNode);
        auto* literal = dynamic_cast<const Literal*>(valueNode);
        if (!column || !literal || column->column != primary->name) {
            return std::nullopt;
        }
        if (!column->table.empty() &&
            column->table != table && column->table != alias) {
            return std::nullopt;
        }
        EvalRow empty;
        return coerceValue(*primary, eval(valueNode, empty));
    };
    auto result = match(binary->left.get(), binary->right.get());
    return result ? result : match(binary->right.get(), binary->left.get());
}

Value NativeEngine::eval(const ASTNode* expression,
                         const EvalRow& row) const {
    if (!expression) return Value::null();
    if (auto* literal = dynamic_cast<const Literal*>(expression)) {
        switch (literal->kind) {
            case LiteralKind::INT:
                return Value(static_cast<std::int64_t>(literal->ival));
            case LiteralKind::FLOAT:
                return Value(literal->fval);
            case LiteralKind::STRING:
                return Value(literal->sval);
            case LiteralKind::NUL:
                return Value::null();
            case LiteralKind::BOOL:
                return Value(literal->bval);
        }
    }
    if (auto* column = dynamic_cast<const ColumnRef*>(expression)) {
        return resolveColumn(*column, row);
    }
    if (auto* binary = dynamic_cast<const BinaryOp*>(expression)) {
        return applyBinary(binary->op,
                           eval(binary->left.get(), row),
                           eval(binary->right.get(), row));
    }
    if (auto* unary = dynamic_cast<const UnaryOp*>(expression)) {
        return applyUnary(unary->op, eval(unary->operand.get(), row));
    }
    if (dynamic_cast<const Wildcard*>(expression)) {
        throw std::runtime_error("Wildcard is not a scalar expression");
    }
    if (auto* function = dynamic_cast<const FunctionCall*>(expression)) {
        if (hasAggregate(function)) {
            throw std::runtime_error(
                "Aggregate used outside grouped evaluation");
        }
        throw std::runtime_error("Unknown scalar function: " + function->name);
    }
    throw std::runtime_error("Unsupported native expression");
}

Value NativeEngine::evalGrouped(
    const ASTNode* expression,
    const std::vector<EvalRow>& rows) const {
    if (!expression) return Value::null();
    if (auto* function = dynamic_cast<const FunctionCall*>(expression)) {
        if (hasAggregate(function)) return aggregate(*function, rows);
    }
    if (auto* binary = dynamic_cast<const BinaryOp*>(expression)) {
        return applyBinary(binary->op,
                           evalGrouped(binary->left.get(), rows),
                           evalGrouped(binary->right.get(), rows));
    }
    if (auto* unary = dynamic_cast<const UnaryOp*>(expression)) {
        return applyUnary(unary->op,
                          evalGrouped(unary->operand.get(), rows));
    }
    if (rows.empty()) return Value::null();
    return eval(expression, rows.front());
}

Value NativeEngine::aggregate(
    const FunctionCall& function,
    const std::vector<EvalRow>& rows) const {
    const std::string& name = function.name;
    if (name == "headcount") {
        if (function.args.empty() ||
            dynamic_cast<const Wildcard*>(function.args.front().get())) {
            return Value(static_cast<std::int64_t>(rows.size()));
        }
        std::int64_t count = 0;
        std::unordered_set<Value, ValueHash> distinct;
        for (const auto& row : rows) {
            Value value = eval(function.args.front().get(), row);
            if (value.isNull()) continue;
            if (!function.distinct || distinct.insert(value).second) ++count;
        }
        return Value(count);
    }
    if (function.args.empty()) {
        throw std::runtime_error("Aggregate requires an argument");
    }
    std::vector<Value> values;
    for (const auto& row : rows) {
        Value value = eval(function.args.front().get(), row);
        if (!value.isNull()) values.push_back(std::move(value));
    }
    if (values.empty()) return Value::null();

    if (name == "stack" || name == "mid") {
        double sum = 0.0;
        bool allIntegers = true;
        for (const auto& value : values) {
            if (!value.isNumeric()) {
                throw std::runtime_error("Numeric aggregate on non-number");
            }
            sum += value.asReal();
            allIntegers = allIntegers &&
                          value.type() == ValueType::Integer;
        }
        if (name == "mid") {
            return Value(sum / static_cast<double>(values.size()));
        }
        if (allIntegers) {
            return Value(static_cast<std::int64_t>(sum));
        }
        return Value(sum);
    }
    if (name == "goat" || name == "L") {
        Value result = values.front();
        for (std::size_t index = 1; index < values.size(); ++index) {
            const int comparison = values[index].compare(result);
            if ((name == "goat" && comparison > 0) ||
                (name == "L" && comparison < 0)) {
                result = values[index];
            }
        }
        return result;
    }
    if (isLoneWolfName(name)) {
        std::vector<double> samples;
        samples.reserve(values.size());
        for (const auto& value : values) {
            if (!value.isNumeric()) {
                throw std::runtime_error(
                    "LONE-WOLF requires numeric operands");
            }
            samples.push_back(value.asReal());
        }
        return Value(countLoneWolves(samples));
    }
    if (name == "mid-fr") {
        std::sort(values.begin(), values.end(),
                  [](const Value& left, const Value& right) {
                      return left.compare(right) < 0;
                  });
        const std::size_t middle = values.size() / 2;
        if (values.size() % 2 == 1) return values[middle];
        if (values[middle - 1].isNumeric() && values[middle].isNumeric()) {
            return Value((values[middle - 1].asReal() +
                          values[middle].asReal()) / 2.0);
        }
        return values[middle - 1];
    }
    if (name == "percent-check") {
        if (function.args.size() < 2) {
            throw std::runtime_error("percent-check requires percentile");
        }
        std::sort(values.begin(), values.end(),
                  [](const Value& left, const Value& right) {
                      return left.compare(right) < 0;
                  });
        Value percentile =
            eval(function.args[1].get(), rows.front());
        const double pct = std::clamp(percentile.asReal(), 0.0, 100.0);
        const std::size_t rank = static_cast<std::size_t>(
            std::ceil(values.size() * pct / 100.0));
        return values[std::max<std::size_t>(1, rank) - 1];
    }
    throw std::runtime_error("Unsupported aggregate: " + name);
}

bool NativeEngine::hasAggregate(const ASTNode* expression) const {
    if (!expression) return false;
    if (auto* function = dynamic_cast<const FunctionCall*>(expression)) {
        const std::string& name = function->name;
        return name == "headcount" || name == "stack" || name == "mid" ||
               name == "goat" || name == "L" || name == "mid-fr" ||
               name == "percent-check" || isLoneWolfName(name);
    }
    if (auto* binary = dynamic_cast<const BinaryOp*>(expression)) {
        return hasAggregate(binary->left.get()) ||
               hasAggregate(binary->right.get());
    }
    if (auto* unary = dynamic_cast<const UnaryOp*>(expression)) {
        return hasAggregate(unary->operand.get());
    }
    return false;
}

bool NativeEngine::selectHasAggregate(
    const SelectStmt& statement) const {
    for (const auto& expression : statement.columns) {
        if (hasAggregate(expression.get())) return true;
    }
    return hasAggregate(statement.having.get());
}

Value NativeEngine::applyBinary(const std::string& op,
                                const Value& left,
                                const Value& right) {
    if (op == "AND") return Value(left.asBoolean() && right.asBoolean());
    if (op == "OR") return Value(left.asBoolean() || right.asBoolean());
    if (left.isNull() || right.isNull()) return Value::null();

    if (op == "=") return Value(left == right);
    if (op == "!=") return Value(left != right);
    if (op == "<") return Value(left.compare(right) < 0);
    if (op == ">") return Value(left.compare(right) > 0);
    if (op == "<=") return Value(left.compare(right) <= 0);
    if (op == ">=") return Value(left.compare(right) >= 0);
    if (op == "||") return Value(left.toString() + right.toString());

    if (!left.isNumeric() || !right.isNumeric()) {
        throw std::runtime_error("Arithmetic requires numeric operands");
    }
    if (op == "+") {
        if (left.type() == ValueType::Integer &&
            right.type() == ValueType::Integer) {
            return Value(left.asInteger() + right.asInteger());
        }
        return Value(left.asReal() + right.asReal());
    }
    if (op == "-") {
        if (left.type() == ValueType::Integer &&
            right.type() == ValueType::Integer) {
            return Value(left.asInteger() - right.asInteger());
        }
        return Value(left.asReal() - right.asReal());
    }
    if (op == "*") {
        if (left.type() == ValueType::Integer &&
            right.type() == ValueType::Integer) {
            return Value(left.asInteger() * right.asInteger());
        }
        return Value(left.asReal() * right.asReal());
    }
    if (op == "/") {
        if (right.asReal() == 0.0) return Value::null();
        if (left.type() == ValueType::Integer &&
            right.type() == ValueType::Integer) {
            return Value(left.asInteger() / right.asInteger());
        }
        return Value(left.asReal() / right.asReal());
    }
    throw std::runtime_error("Unsupported binary operator: " + op);
}

Value NativeEngine::applyUnary(const std::string& op,
                               const Value& value) {
    if (op == "NOT") return Value(!value.asBoolean());
    if (value.isNull()) return Value::null();
    if (op == "-") {
        if (value.type() == ValueType::Integer) {
            return Value(-value.asInteger());
        }
        if (value.isNumeric()) return Value(-value.asReal());
    }
    throw std::runtime_error("Unsupported unary operator: " + op);
}

Value NativeEngine::resolveColumn(const ColumnRef& column,
                                  const EvalRow& row) const {
    std::optional<Value> result;
    if (!row.columns) {
        throw std::runtime_error("Expression row has no schema");
    }
    for (std::size_t index = 0; index < row.columns->size(); ++index) {
        const auto& candidate = (*row.columns)[index];
        const bool qualifierMatches =
            column.table.empty() ||
            column.table == candidate.table ||
            (!candidate.alias.empty() &&
             column.table == candidate.alias);
        if (candidate.name != column.column || !qualifierMatches) continue;
        if (result && column.table.empty()) {
            throw std::runtime_error(
                "Ambiguous column: " + column.column);
        }
        result = row.values[index];
    }
    if (!result) {
        throw std::runtime_error("Unknown column: " +
            (column.table.empty() ? "" : column.table + ".") +
            column.column);
    }
    return *result;
}

Value NativeEngine::resolveOutputColumn(
    const ColumnRef& column,
    const std::vector<std::string>& names,
    const Tuple& values) const {
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == column.column) return values[index];
    }
    throw std::runtime_error("Unknown output column: " + column.column);
}

std::vector<std::string> NativeEngine::outputNames(
    const SelectStmt& statement,
    const EvalRow* sample) const {
    std::vector<std::string> names;
    for (std::size_t index = 0; index < statement.columns.size(); ++index) {
        const auto* expression = statement.columns[index].get();
        if (auto* wildcard = dynamic_cast<const Wildcard*>(expression)) {
            if (sample) {
                for (const auto& column : *sample->columns) {
                    if (wildcard->table.empty() ||
                        wildcard->table == column.table ||
                        wildcard->table == column.alias) {
                        names.push_back(column.name);
                    }
                }
            } else {
                auto appendTable = [&](const std::string& table,
                                       const std::string& alias) {
                    if (!wildcard->table.empty() &&
                        wildcard->table != table &&
                        wildcard->table != alias) {
                        return;
                    }
                    const auto* metadata = catalog_.getTable(table);
                    if (!metadata) return;
                    for (const auto& column : metadata->columns) {
                        names.push_back(column.name);
                    }
                };
                appendTable(statement.fromTable, statement.fromAlias);
                for (const auto& join : statement.joins) {
                    appendTable(join.table, join.alias);
                }
            }
        } else {
            names.push_back(expressionAlias(expression, index));
        }
    }
    return names;
}

Tuple NativeEngine::project(const SelectStmt& statement,
                            const EvalRow& row,
                            const std::vector<EvalRow>* allRows) const {
    Tuple result;
    for (const auto& expression : statement.columns) {
        if (auto* wildcard =
                dynamic_cast<const Wildcard*>(expression.get())) {
            for (std::size_t index = 0;
                 index < row.columns->size();
                 ++index) {
                const auto& column = (*row.columns)[index];
                if (wildcard->table.empty() ||
                    wildcard->table == column.table ||
                    wildcard->table == column.alias) {
                    result.push_back(row.values[index]);
                }
            }
        } else if (auto* window =
                       dynamic_cast<const WindowFunc*>(expression.get())) {
            if (!allRows) {
                throw std::runtime_error(
                    "Window function requires an input relation");
            }
            result.push_back(windowValue(*window, row, *allRows));
        } else {
            result.push_back(eval(expression.get(), row));
        }
    }
    return result;
}

Tuple NativeEngine::projectGrouped(
    const SelectStmt& statement,
    const std::vector<EvalRow>& rows) const {
    Tuple result;
    for (const auto& expression : statement.columns) {
        if (dynamic_cast<const Wildcard*>(expression.get())) {
            throw std::runtime_error(
                "Wildcard is not supported in aggregate projection");
        }
        result.push_back(evalGrouped(expression.get(), rows));
    }
    return result;
}

std::vector<Value> NativeEngine::orderKeys(
    const SelectStmt& statement,
    const EvalRow& row,
    const std::vector<std::string>& names,
    const Tuple& projected) const {
    std::vector<Value> keys;
    for (const auto& order : statement.orderBy) {
        try {
            keys.push_back(eval(order.expr.get(), row));
        } catch (const std::runtime_error&) {
            auto* column =
                dynamic_cast<const ColumnRef*>(order.expr.get());
            if (!column) throw;
            keys.push_back(resolveOutputColumn(*column, names, projected));
        }
    }
    return keys;
}

std::vector<Value> NativeEngine::orderKeysGrouped(
    const SelectStmt& statement,
    const std::vector<EvalRow>& rows,
    const std::vector<std::string>& names,
    const Tuple& projected) const {
    std::vector<Value> keys;
    for (const auto& order : statement.orderBy) {
        try {
            keys.push_back(evalGrouped(order.expr.get(), rows));
        } catch (const std::runtime_error&) {
            auto* column =
                dynamic_cast<const ColumnRef*>(order.expr.get());
            if (!column) throw;
            keys.push_back(resolveOutputColumn(*column, names, projected));
        }
    }
    return keys;
}

Tuple NativeEngine::validateAndCoerce(
    const TableMeta& table,
    Tuple tuple,
    const std::optional<RowId>& replacing) {
    if (tuple.size() != table.columns.size()) {
        throw std::runtime_error("Tuple width does not match schema");
    }
    for (std::size_t index = 0; index < tuple.size(); ++index) {
        tuple[index] = coerceValue(table.columns[index], tuple[index]);
        if ((table.columns[index].not_null ||
             table.columns[index].primary_key) &&
            tuple[index].isNull()) {
            throw std::runtime_error(
                "NULL violates constraint on " + table.columns[index].name);
        }
    }

    for (std::size_t column = 0;
         column < table.columns.size();
         ++column) {
        if (!table.columns[column].primary_key) continue;
        if (!replacing) {
            if (primaryIndex(table.name).find(tuple[column])) {
                throw std::runtime_error(
                    "Duplicate primary key for table " + table.name);
            }
            continue;
        }
        for (const auto& row : heap(table.name).scan()) {
            if (row.id == *replacing) continue;
            if (row.values[column] == tuple[column]) {
                throw std::runtime_error(
                    "Duplicate primary key for table " + table.name);
            }
        }
    }
    validateForeignKeys(table, tuple);
    return tuple;
}

void NativeEngine::validateForeignKeys(
    const TableMeta& table,
    const Tuple& tuple) {
    for (std::size_t index = 0;
         index < table.columns.size();
         ++index) {
        const auto& column = table.columns[index];
        if (column.fk_table.empty() || tuple[index].isNull()) continue;
        const auto* foreign = catalog_.getTable(column.fk_table);
        if (!foreign) {
            throw std::runtime_error(
                "Foreign key table does not exist: " + column.fk_table);
        }
        auto foundColumn = std::find_if(
            foreign->columns.begin(), foreign->columns.end(),
            [&](const ColumnMeta& candidate) {
                return candidate.name == column.fk_col;
            });
        if (foundColumn == foreign->columns.end()) {
            throw std::runtime_error(
                "Foreign key column does not exist: " + column.fk_col);
        }
        const auto position = static_cast<std::size_t>(
            std::distance(foreign->columns.begin(), foundColumn));
        bool found = false;
        for (const auto& row : heap(foreign->name).scan()) {
            if (row.values[position] == tuple[index]) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(
                "Foreign key constraint failed on " + column.name);
        }
    }
}

void NativeEngine::validateNoIncomingReferences(
    const TableMeta& table,
    const Tuple& tuple,
    const Tuple* replacement) {
    for (const auto& referencingTableName : catalog_.tableNames()) {
        const auto* referencing =
            catalog_.getTable(referencingTableName);
        if (!referencing) continue;
        for (std::size_t foreignIndex = 0;
             foreignIndex < referencing->columns.size();
             ++foreignIndex) {
            const auto& foreignColumn =
                referencing->columns[foreignIndex];
            if (foreignColumn.fk_table != table.name) continue;
            auto target = std::find_if(
                table.columns.begin(), table.columns.end(),
                [&](const ColumnMeta& column) {
                    return column.name == foreignColumn.fk_col;
                });
            if (target == table.columns.end()) continue;
            const auto targetIndex = static_cast<std::size_t>(
                std::distance(table.columns.begin(), target));
            if (replacement &&
                tuple[targetIndex] == (*replacement)[targetIndex]) {
                continue;
            }
            for (const auto& row : heap(referencing->name).scan()) {
                if (!row.values[foreignIndex].isNull() &&
                    row.values[foreignIndex] == tuple[targetIndex]) {
                    throw std::runtime_error(
                        "Row is referenced by " + referencing->name +
                        "." + foreignColumn.name);
                }
            }
        }
    }
}

Value NativeEngine::coerceValue(const ColumnMeta& column,
                                const Value& value) {
    if (value.isNull()) return value;
    const std::string type = upper(column.type);
    if (type == "INTEGER" || type == "INT") {
        if (!value.isNumeric()) {
            throw std::runtime_error(
                "Expected INTEGER for " + column.name);
        }
        return Value(value.asInteger());
    }
    if (type == "REAL" || type == "FLOAT" || type == "DOUBLE") {
        if (!value.isNumeric()) {
            throw std::runtime_error("Expected REAL for " + column.name);
        }
        return Value(value.asReal());
    }
    if (type == "TEXT" || type == "VARCHAR" || type == "STRING") {
        return Value(value.toString());
    }
    if (type == "BLOB") {
        if (value.type() != ValueType::Blob) {
            throw std::runtime_error("Expected BLOB for " + column.name);
        }
        return value;
    }
    throw std::runtime_error("Unsupported column type: " + column.type);
}

Value NativeEngine::windowValue(
    const WindowFunc& window,
    const EvalRow& row,
    const std::vector<EvalRow>& allRows) const {
    if (window.funcName != "RANK") {
        throw std::runtime_error(
            "Unsupported window function: " + window.funcName);
    }

    auto samePartition = [&](const EvalRow& candidate) {
        for (const auto& expression : window.partition_by) {
            if (eval(expression.get(), candidate) !=
                eval(expression.get(), row)) {
                return false;
            }
        }
        return true;
    };

    auto precedes = [&](const EvalRow& candidate) {
        for (const auto& order : window.order_by) {
            const int comparison =
                eval(order.expr.get(), candidate).compare(
                    eval(order.expr.get(), row));
            if (comparison == 0) continue;
            return order.asc ? comparison < 0 : comparison > 0;
        }
        return false;
    };

    std::int64_t rank = 1;
    for (const auto& candidate : allRows) {
        if (samePartition(candidate) && precedes(candidate)) ++rank;
    }
    return Value(rank);
}

bool NativeEngine::tupleEqual(const Tuple& left, const Tuple& right) {
    return left == right;
}

std::size_t NativeEngine::tupleHash(const Tuple& tuple) {
    return TupleKeyHash{}(tuple);
}

std::vector<std::uint8_t> NativeEngine::readFile(
    const std::filesystem::path& path) const {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void NativeEngine::restoreFile(
    const std::filesystem::path& path,
    const std::optional<std::vector<std::uint8_t>>& bytes) {
    mappedHeaps_.clear();
    bufferPool_.invalidateFile(path);
    if (!bytes) {
        std::error_code error;
        std::filesystem::remove(path, error);
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes->data()),
                 static_cast<std::streamsize>(bytes->size()));
    if (!output) {
        throw std::runtime_error("Failed restoring heap file");
    }
}

void NativeEngine::reloadCatalog() {
    catalog_ = Catalog((root_ / "catalog.json").string());
    catalog_.load();
}
