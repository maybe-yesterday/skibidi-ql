#include "native_engine.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
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
            seed ^= value.hash() + 0x9e3779b97f4a7c15ULL +
                    (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

struct TupleKeyEqual {
    bool operator()(const Tuple& left, const Tuple& right) const {
        return left == right;
    }
};

class BloomFilter {
public:
    explicit BloomFilter(std::size_t expectedValues) {
        std::size_t bits = 64;
        const std::size_t wanted =
            std::max<std::size_t>(64, expectedValues * 12);
        while (bits < wanted) bits <<= 1;
        bitMask_ = bits - 1;
        words_.assign((bits + 63) / 64, 0);
    }

    void add(const Value& value) {
        const auto hash = value.hash();
        set(hash);
        set(hash + mix(hash));
        set(hash + mix(hash ^ 0x9e3779b97f4a7c15ULL));
    }

    bool mayContain(const Value& value) const {
        const auto hash = value.hash();
        return test(hash) &&
               test(hash + mix(hash)) &&
               test(hash + mix(hash ^ 0x9e3779b97f4a7c15ULL));
    }

private:
    std::vector<std::uint64_t> words_;
    std::size_t bitMask_ = 63;

    static std::size_t mix(std::size_t value) {
        value ^= value >> 33;
        value *= static_cast<std::size_t>(0xff51afd7ed558ccdULL);
        value ^= value >> 33;
        value *= static_cast<std::size_t>(0xc4ceb9fe1a85ec53ULL);
        value ^= value >> 33;
        return value | 1U;
    }

    void set(std::size_t hash) {
        const auto bit = hash & bitMask_;
        words_[bit / 64] |=
            static_cast<std::uint64_t>(1) << (bit % 64);
    }

    bool test(std::size_t hash) const {
        const auto bit = hash & bitMask_;
        return (words_[bit / 64] &
                (static_cast<std::uint64_t>(1) << (bit % 64))) != 0;
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

std::int64_t integerLiteral(const ASTNode* expression,
                            std::int64_t fallback) {
    auto* literal = dynamic_cast<const Literal*>(expression);
    if (!literal) return fallback;
    if (literal->kind == LiteralKind::INT) return literal->ival;
    if (literal->kind == LiteralKind::FLOAT) {
        return static_cast<std::int64_t>(literal->fval);
    }
    return fallback;
}

template <typename T>
T readRawPod(const std::uint8_t* data,
             std::size_t length,
             std::size_t& offset) {
    if (offset + sizeof(T) > length) {
        throw std::runtime_error("Corrupt tuple payload");
    }
    T value{};
    std::memcpy(&value, data + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

struct RawField {
    ValueType type = ValueType::Null;
    bool isNull = true;
    std::int64_t integer = 0;
    double real = 0.0;
    bool boolean = false;
    std::string text;
    Value::Blob blob;

    bool numeric() const {
        return !isNull &&
               (type == ValueType::Integer ||
                type == ValueType::Real ||
                type == ValueType::Boolean);
    }

    double asReal() const {
        if (type == ValueType::Real) return real;
        if (type == ValueType::Integer) {
            return static_cast<double>(integer);
        }
        if (type == ValueType::Boolean) return boolean ? 1.0 : 0.0;
        throw std::runtime_error("Raw field is not numeric");
    }

    Value toValue() const {
        if (isNull) return Value::null();
        switch (type) {
            case ValueType::Integer:
                return Value(integer);
            case ValueType::Real:
                return Value(real);
            case ValueType::Text:
                return Value(text);
            case ValueType::Boolean:
                return Value(boolean);
            case ValueType::Blob:
                return Value(blob);
            case ValueType::Null:
                return Value::null();
        }
        return Value::null();
    }
};

Value literalValue(const Literal& literal) {
    switch (literal.kind) {
        case LiteralKind::INT:
            return Value(static_cast<std::int64_t>(literal.ival));
        case LiteralKind::FLOAT:
            return Value(literal.fval);
        case LiteralKind::STRING:
            return Value(literal.sval);
        case LiteralKind::NUL:
            return Value::null();
        case LiteralKind::BOOL:
            return Value(literal.bval);
    }
    return Value::null();
}

void decodeRawProjectedInto(
    const std::uint8_t* data,
    std::size_t length,
    const std::vector<std::size_t>& columns,
    std::vector<RawField>& fields) {
    fields.clear();
    if (columns.empty()) return;

    std::size_t offset = 0;
    const std::uint16_t count =
        readRawPod<std::uint16_t>(data, length, offset);
    if (!columns.empty() &&
        (columns.back() >= count ||
         !std::is_sorted(columns.begin(), columns.end()) ||
             std::adjacent_find(columns.begin(), columns.end()) !=
             columns.end())) {
        throw std::runtime_error("Invalid projected tuple columns");
    }

    fields.resize(columns.size());
    std::size_t projected = 0;
    for (std::uint16_t index = 0; index < count; ++index) {
        if (offset >= length) {
            throw std::runtime_error("Corrupt tuple type tag");
        }
        const auto type = static_cast<ValueType>(data[offset++]);
        const bool materialize =
            projected < columns.size() &&
            columns[projected] == index;
        RawField field;
        field.type = type;
        field.isNull = type == ValueType::Null;
        switch (type) {
            case ValueType::Null:
                break;
            case ValueType::Integer:
                field.integer =
                    readRawPod<std::int64_t>(data, length, offset);
                break;
            case ValueType::Real:
                field.real = readRawPod<double>(data, length, offset);
                break;
            case ValueType::Boolean:
                if (offset >= length) {
                    throw std::runtime_error(
                        "Corrupt boolean tuple field");
                }
                field.boolean = data[offset++] != 0;
                break;
            case ValueType::Text: {
                const auto textLength =
                    readRawPod<std::uint32_t>(data, length, offset);
                if (offset + textLength > length) {
                    throw std::runtime_error(
                        "Corrupt text tuple field");
                }
                if (materialize) {
                    field.text.assign(
                        reinterpret_cast<const char*>(data + offset),
                        textLength);
                }
                offset += textLength;
                break;
            }
            case ValueType::Blob: {
                const auto blobLength =
                    readRawPod<std::uint32_t>(data, length, offset);
                if (offset + blobLength > length) {
                    throw std::runtime_error(
                        "Corrupt blob tuple field");
                }
                if (materialize) {
                    field.blob.assign(data + offset,
                                      data + offset + blobLength);
                }
                offset += blobLength;
                break;
            }
            default:
                throw std::runtime_error("Unknown tuple field type");
        }
        if (materialize) {
            fields[projected] = std::move(field);
            ++projected;
            if (projected == columns.size()) break;
        }
    }
}

RawField decodeRawColumn(const std::uint8_t* data,
                         std::size_t length,
                         std::size_t column) {
    std::size_t offset = 0;
    const std::uint16_t count =
        readRawPod<std::uint16_t>(data, length, offset);
    if (column >= count) {
        throw std::runtime_error("Invalid projected tuple column");
    }

    for (std::uint16_t index = 0; index < count; ++index) {
        if (offset >= length) {
            throw std::runtime_error("Corrupt tuple type tag");
        }
        const auto type = static_cast<ValueType>(data[offset++]);
        const bool materialize = index == column;
        RawField field;
        field.type = type;
        field.isNull = type == ValueType::Null;
        switch (type) {
            case ValueType::Null:
                break;
            case ValueType::Integer:
                field.integer =
                    readRawPod<std::int64_t>(data, length, offset);
                break;
            case ValueType::Real:
                field.real = readRawPod<double>(data, length, offset);
                break;
            case ValueType::Boolean:
                if (offset >= length) {
                    throw std::runtime_error(
                        "Corrupt boolean tuple field");
                }
                field.boolean = data[offset++] != 0;
                break;
            case ValueType::Text: {
                const auto textLength =
                    readRawPod<std::uint32_t>(data, length, offset);
                if (offset + textLength > length) {
                    throw std::runtime_error(
                        "Corrupt text tuple field");
                }
                if (materialize) {
                    field.text.assign(
                        reinterpret_cast<const char*>(data + offset),
                        textLength);
                }
                offset += textLength;
                break;
            }
            case ValueType::Blob: {
                const auto blobLength =
                    readRawPod<std::uint32_t>(data, length, offset);
                if (offset + blobLength > length) {
                    throw std::runtime_error(
                        "Corrupt blob tuple field");
                }
                if (materialize) {
                    field.blob.assign(data + offset,
                                      data + offset + blobLength);
                }
                offset += blobLength;
                break;
            }
            default:
                throw std::runtime_error("Unknown tuple field type");
        }
        if (materialize) return field;
    }
    throw std::runtime_error("Invalid projected tuple column");
}

int compareRawToValue(const RawField& left, const Value& right) {
    if (left.isNull || right.isNull()) {
        if (left.isNull && right.isNull()) return 0;
        return left.isNull ? -1 : 1;
    }
    if (left.numeric() && right.isNumeric()) {
        const double l = left.asReal();
        const double r = right.asReal();
        return l < r ? -1 : (l > r ? 1 : 0);
    }
    return left.toValue().compare(right);
}

bool comparisonMatches(const std::string& op, int order) {
    if (op == "=") return order == 0;
    if (op == "!=") return order != 0;
    if (op == "<") return order < 0;
    if (op == ">") return order > 0;
    if (op == "<=") return order <= 0;
    if (op == ">=") return order >= 0;
    throw std::runtime_error("Unsupported comparison operator: " + op);
}

bool isLoneWolfName(const std::string& name) {
    return name == "LONE-WOLF" || name == "lone-wolf";
}

bool isSimpleAggregateName(const std::string& name) {
    return name == "headcount" || name == "stack" || name == "mid" ||
           name == "goat" || name == "L" || isLoneWolfName(name);
}

std::int64_t countLoneWolves(const std::vector<double>& samples) {
    if (samples.size() < 2) return 0;
    double sum = 0.0;
    double sumSquares = 0.0;
    for (const double value : samples) {
        sum += value;
        sumSquares += value * value;
    }
    const double count = static_cast<double>(samples.size());
    const double mean = sum / count;
    const double variance = std::max(0.0, sumSquares / count - mean * mean);
    const double sigma = std::sqrt(variance);
    if (sigma == 0.0) return 0;
    std::int64_t outliers = 0;
    const double threshold = sigma * 3.0;
    for (const double value : samples) {
        if (std::fabs(value - mean) > threshold) ++outliers;
    }
    return outliers;
}

std::string normalizeColumnPredicateOp(std::string op,
                                       bool columnOnLeft) {
    if (columnOnLeft) return op;
    if (op == "<") return ">";
    if (op == ">") return "<";
    if (op == "<=") return ">=";
    if (op == ">=") return "<=";
    return op;
}

} // namespace

NativeEngine::TypedVector::TypedVector(ValueType valueType)
    : type(valueType) {
    switch (type) {
        case ValueType::Null:
            storage = std::monostate{};
            break;
        case ValueType::Integer:
            storage = std::vector<std::int64_t>{};
            break;
        case ValueType::Real:
            storage = std::vector<double>{};
            break;
        case ValueType::Text:
            storage = std::vector<std::string>{};
            break;
        case ValueType::Boolean:
            storage = std::vector<std::uint8_t>{};
            break;
        case ValueType::Blob:
            storage = std::vector<Value::Blob>{};
            break;
    }
}

void NativeEngine::TypedVector::reserve(std::size_t capacity) {
    nullBitmap.reserve((capacity + 63) / 64);
    switch (type) {
        case ValueType::Integer:
            std::get<std::vector<std::int64_t>>(storage).reserve(capacity);
            break;
        case ValueType::Real:
            std::get<std::vector<double>>(storage).reserve(capacity);
            break;
        case ValueType::Text:
            std::get<std::vector<std::string>>(storage).reserve(capacity);
            break;
        case ValueType::Boolean:
            std::get<std::vector<std::uint8_t>>(storage).reserve(capacity);
            break;
        case ValueType::Blob:
            std::get<std::vector<Value::Blob>>(storage).reserve(capacity);
            break;
        case ValueType::Null:
            break;
    }
}

void NativeEngine::TypedVector::append(const Value& input) {
    if (nullBitmap.size() <= size / 64) {
        nullBitmap.push_back(0);
    }
    if (input.isNull()) {
        nullBitmap[size / 64] |=
            static_cast<std::uint64_t>(1) << (size % 64);
    } else if (type == ValueType::Null) {
        type = input.type();
        switch (type) {
            case ValueType::Integer:
                storage = std::vector<std::int64_t>(size, 0);
                break;
            case ValueType::Real:
                storage = std::vector<double>(size, 0.0);
                break;
            case ValueType::Text:
                storage = std::vector<std::string>(size);
                break;
            case ValueType::Boolean:
                storage = std::vector<std::uint8_t>(size, 0);
                break;
            case ValueType::Blob:
                storage = std::vector<Value::Blob>(size);
                break;
            case ValueType::Null:
                break;
        }
    } else if (type == ValueType::Integer &&
               input.type() == ValueType::Real) {
        const auto& integers =
            std::get<std::vector<std::int64_t>>(storage);
        std::vector<double> reals;
        reals.reserve(integers.size() + 1);
        for (const auto value : integers) {
            reals.push_back(static_cast<double>(value));
        }
        storage = std::move(reals);
        type = ValueType::Real;
    }

    switch (type) {
        case ValueType::Null:
            break;
        case ValueType::Integer:
            std::get<std::vector<std::int64_t>>(storage).push_back(
                input.isNull() ? 0 : input.asInteger());
            break;
        case ValueType::Real:
            std::get<std::vector<double>>(storage).push_back(
                input.isNull() ? 0.0 : input.asReal());
            break;
        case ValueType::Text:
            std::get<std::vector<std::string>>(storage).push_back(
                input.isNull() ? std::string{} : input.toString());
            break;
        case ValueType::Boolean:
            std::get<std::vector<std::uint8_t>>(storage).push_back(
                input.isNull() ? 0 :
                static_cast<std::uint8_t>(input.asBoolean()));
            break;
        case ValueType::Blob:
            std::get<std::vector<Value::Blob>>(storage).push_back(
                input.isNull() ? Value::Blob{} : input.asBlob());
            break;
    }
    ++size;
}

bool NativeEngine::TypedVector::isNull(std::size_t index) const {
    if (index >= size) throw std::out_of_range("Vector row out of range");
    return (nullBitmap[index / 64] &
            (static_cast<std::uint64_t>(1) << (index % 64))) != 0;
}

Value NativeEngine::TypedVector::value(std::size_t index) const {
    if (isNull(index) || type == ValueType::Null) return Value::null();
    switch (type) {
        case ValueType::Integer:
            return Value(
                std::get<std::vector<std::int64_t>>(storage)[index]);
        case ValueType::Real:
            return Value(std::get<std::vector<double>>(storage)[index]);
        case ValueType::Text:
            return Value(
                std::get<std::vector<std::string>>(storage)[index]);
        case ValueType::Boolean:
            return Value(
                std::get<std::vector<std::uint8_t>>(storage)[index] != 0);
        case ValueType::Blob:
            return Value(
                std::get<std::vector<Value::Blob>>(storage)[index]);
        case ValueType::Null:
            return Value::null();
    }
    return Value::null();
}

NativeEngine::NativeEngine(std::filesystem::path databasePath,
                           std::size_t bufferPages)
    : root_(std::move(databasePath)),
      catalog_((root_ / "catalog.json").string()),
      bufferPool_(bufferPages) {
    if (root_ == ":memory:") {
        temporary_ = true;
        const auto stamp =
            std::chrono::high_resolution_clock::now()
                .time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("skibidi-native-" + std::to_string(stamp));
        catalog_ = Catalog((root_ / "catalog.json").string());
    }
    root_ = std::filesystem::absolute(root_).lexically_normal();
    catalog_ = Catalog((root_ / "catalog.json").string());
    std::filesystem::create_directories(root_ / "tables");
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
    if (transactionActive_) {
        throw std::runtime_error("A transaction is already active");
    }
    flush();
    transactionSnapshot_.clear();
    if (std::filesystem::exists(root_)) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root_)) {
            if (!entry.is_regular_file()) continue;
            const auto relative =
                std::filesystem::relative(entry.path(), root_).generic_string();
            transactionSnapshot_[relative] = readFile(entry.path());
        }
    }
    transactionActive_ = true;
}

void NativeEngine::commitTransaction() {
    if (!transactionActive_) {
        throw std::runtime_error("No transaction is active");
    }
    flush();
    transactionSnapshot_.clear();
    transactionActive_ = false;
}

void NativeEngine::rollbackTransaction() {
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
}

NativeQueryResult NativeEngine::execute(const ASTNode* statement) {
    if (!statement) return {};
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
    throw std::runtime_error("Unsupported native statement");
}

void NativeEngine::flush() {
    bufferPool_.flushAll();
    catalog_.save();
}

NativeEngineStats NativeEngine::stats() const {
    auto result = stats_;
    result.residentPages = bufferPool_.residentPages();
    result.bufferCapacityPages = bufferPool_.capacityPages();
    result.bufferPageReads = bufferPool_.pageReads();
    result.bufferEvictions = bufferPool_.evictions();
    return result;
}

void NativeEngine::resetStats() {
    stats_ = {};
    bufferPool_.resetStats();
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

    auto file = heap(statement.table);
    file.create();
    try {
        catalog_.addTable(table);
        catalog_.save();
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

    try {
        invalidateMappedHeap(statement.table);
        heap(statement.table).drop();
        primaryIndexes_.erase(statement.table);
        tableStatistics_.erase(statement.table);
        catalog_.removeTable(statement.table);
        catalog_.save();
    } catch (...) {
        catalog_.addTable(backup);
        restoreFile(path, bytes);
        catalog_.save();
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

NativeQueryResult NativeEngine::executeSelect(
    const SelectStmt& statement) {
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

    if (!plannedRows) for (const auto& join : statement.joins) {
        auto rightScan = scanTable(join.table, join.alias);
        std::vector<EvalRow> right;
        right.reserve(rightScan.size());
        for (auto& item : rightScan) right.push_back(std::move(item.row));
        const auto leftSchema = !rows.empty()
            ? rows.front().columns
            : boundSchema(statement.fromTable, statement.fromAlias);
        const auto rightSchema = !right.empty()
            ? right.front().columns
            : boundSchema(join.table, join.alias);
        const auto joinedSchema =
            combineSchemas(leftSchema, rightSchema);

        std::vector<EvalRow> joined;
        const auto* equality =
            dynamic_cast<const BinaryOp*>(join.condition.get());
        const ColumnRef* leftKey = nullptr;
        const ColumnRef* rightKey = nullptr;
        if (equality && equality->op == "=") {
            auto* first =
                dynamic_cast<const ColumnRef*>(equality->left.get());
            auto* second =
                dynamic_cast<const ColumnRef*>(equality->right.get());
            auto belongsToRight = [&](const ColumnRef* column) {
                return column && !column->table.empty() &&
                       (column->table == join.table ||
                        column->table == join.alias);
            };
            if (belongsToRight(first) && !belongsToRight(second)) {
                rightKey = first;
                leftKey = second;
            } else if (belongsToRight(second) &&
                       !belongsToRight(first)) {
                rightKey = second;
                leftKey = first;
            }
        }

        if (leftKey && rightKey) {
            std::unordered_multimap<Value, const EvalRow*, ValueHash> index;
            BloomFilter bloom(right.size());
            ++stats_.bloomFilterBuilds;
            for (const auto& rightRow : right) {
                Value key = resolveColumn(*rightKey, rightRow);
                if (!key.isNull()) {
                    bloom.add(key);
                    index.emplace(std::move(key), &rightRow);
                }
            }
            for (const auto& leftRow : rows) {
                Value key = resolveColumn(*leftKey, leftRow);
                bool matched = false;
                if (!key.isNull()) {
                    ++stats_.bloomFilterChecks;
                    if (!bloom.mayContain(key)) {
                        ++stats_.bloomFilterRejects;
                    } else {
                    const auto range = index.equal_range(key);
                    ++stats_.hashJoinProbes;
                    for (auto match = range.first;
                         match != range.second;
                        ++match) {
                        EvalRow combined = leftRow;
                        combined.columns = joinedSchema;
                        combined.values.insert(
                            combined.values.end(),
                            match->second->values.begin(),
                            match->second->values.end());
                        joined.push_back(std::move(combined));
                        matched = true;
                    }
                    }
                }
                if (!matched && join.type == JoinType::LEFT) {
                    EvalRow combined = leftRow;
                    combined.columns = joinedSchema;
                    const auto* metadata = catalog_.getTable(join.table);
                    if (metadata) {
                        combined.values.insert(
                            combined.values.end(),
                            metadata->columns.size(), Value::null());
                    }
                    joined.push_back(std::move(combined));
                }
            }
            rows = std::move(joined);
            continue;
        }

        for (const auto& leftRow : rows) {
            bool matched = false;
            for (const auto& rightRow : right) {
                ++stats_.nestedLoopComparisons;
                EvalRow combined = leftRow;
                combined.columns = joinedSchema;
                combined.values.insert(combined.values.end(),
                                       rightRow.values.begin(),
                                       rightRow.values.end());
                if (!join.condition ||
                    eval(join.condition.get(), combined).asBoolean()) {
                    joined.push_back(std::move(combined));
                    matched = true;
                }
            }
            if (!matched && join.type == JoinType::LEFT) {
                EvalRow combined = leftRow;
                combined.columns = joinedSchema;
                if (!right.empty()) {
                    combined.values.insert(
                        combined.values.end(),
                        right.front().values.size(), Value::null());
                } else if (const auto* metadata =
                               catalog_.getTable(join.table)) {
                    combined.values.insert(
                        combined.values.end(),
                        metadata->columns.size(), Value::null());
                }
                joined.push_back(std::move(combined));
            }
        }
        rows = std::move(joined);
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

std::optional<NativeQueryResult>
NativeEngine::executeRowIdSeekJoinAggregateSelect(
    const SelectStmt& statement) {
    if (statement.joins.empty() || statement.distinct ||
        statement.where || statement.having || !statement.orderBy.empty() ||
        statement.limit || statement.offset ||
        !selectHasAggregate(statement)) {
        return std::nullopt;
    }
    for (const auto& join : statement.joins) {
        if (join.type != JoinType::INNER) return std::nullopt;
    }
    for (const auto& expression : statement.columns) {
        if (dynamic_cast<const Wildcard*>(expression.get())) {
            return std::nullopt;
        }
    }

    const auto* baseMetadata = catalog_.getTable(statement.fromTable);
    if (!baseMetadata) {
        throw std::runtime_error(
            "Unknown table: " + statement.fromTable);
    }

    const auto matchesRelation =
        [](const std::string& qualifier,
           const std::string& table,
           const std::string& alias) {
        return !qualifier.empty() &&
               (qualifier == table ||
                (!alias.empty() && qualifier == alias));
    };
    const auto columnIndex =
        [](const TableMeta& metadata,
           const std::string& columnName) -> std::optional<std::size_t> {
        auto found = std::find_if(
            metadata.columns.begin(), metadata.columns.end(),
            [&](const ColumnMeta& column) {
                return column.name == columnName;
            });
        if (found == metadata.columns.end()) return std::nullopt;
        return static_cast<std::size_t>(
            std::distance(metadata.columns.begin(), found));
    };
    const auto primaryKeyIndex =
        [](const TableMeta& metadata) -> std::optional<std::size_t> {
        auto found = std::find_if(
            metadata.columns.begin(), metadata.columns.end(),
            [](const ColumnMeta& column) {
                return column.primary_key;
            });
        if (found == metadata.columns.end()) return std::nullopt;
        return static_cast<std::size_t>(
            std::distance(metadata.columns.begin(), found));
    };

    struct JoinSeekPlan {
        std::string table;
        std::string alias;
        const TableMeta* metadata = nullptr;
        std::size_t primaryKeyColumn = 0;
        std::size_t baseKeyColumn = 0;
        std::set<std::size_t> requiredSet;
        std::vector<std::size_t> requiredColumns;
        std::vector<std::size_t> fieldPositions;
    };

    std::vector<JoinSeekPlan> joins;
    joins.reserve(statement.joins.size());
    for (const auto& join : statement.joins) {
        const auto* joinMetadata = catalog_.getTable(join.table);
        if (!joinMetadata) {
            throw std::runtime_error("Unknown table: " + join.table);
        }
        const auto primary = primaryKeyIndex(*joinMetadata);
        if (!primary) return std::nullopt;

        const auto* equality =
            dynamic_cast<const BinaryOp*>(join.condition.get());
        if (!equality || equality->op != "=") return std::nullopt;
        const auto* left =
            dynamic_cast<const ColumnRef*>(equality->left.get());
        const auto* right =
            dynamic_cast<const ColumnRef*>(equality->right.get());
        if (!left || !right) return std::nullopt;

        const auto baseAlias = statement.fromAlias;
        const bool leftBase = matchesRelation(
            left->table, statement.fromTable, baseAlias);
        const bool rightBase = matchesRelation(
            right->table, statement.fromTable, baseAlias);
        const bool leftJoin =
            matchesRelation(left->table, join.table, join.alias);
        const bool rightJoin =
            matchesRelation(right->table, join.table, join.alias);

        const ColumnRef* baseColumn = nullptr;
        const ColumnRef* joinedColumn = nullptr;
        if (leftBase && rightJoin) {
            baseColumn = left;
            joinedColumn = right;
        } else if (rightBase && leftJoin) {
            baseColumn = right;
            joinedColumn = left;
        } else {
            return std::nullopt;
        }

        const auto baseKey = columnIndex(*baseMetadata, baseColumn->column);
        const auto joinedKey = columnIndex(
            *joinMetadata, joinedColumn->column);
        if (!baseKey || !joinedKey || *joinedKey != *primary) {
            return std::nullopt;
        }

        JoinSeekPlan plan;
        plan.table = join.table;
        plan.alias = join.alias;
        plan.metadata = joinMetadata;
        plan.primaryKeyColumn = *primary;
        plan.baseKeyColumn = *baseKey;
        joins.push_back(std::move(plan));
    }

    enum class SourceKind { Base, Join };
    struct ColumnAccess {
        SourceKind source = SourceKind::Base;
        std::size_t join = 0;
        std::size_t column = 0;
    };
    const auto sameColumn =
        [](const ColumnAccess& left, const ColumnAccess& right) {
        return left.source == right.source &&
               left.join == right.join &&
               left.column == right.column;
    };

    auto resolveColumn =
        [&](const ColumnRef& column) -> std::optional<ColumnAccess> {
        if (!column.table.empty()) {
            if (matchesRelation(column.table,
                                statement.fromTable,
                                statement.fromAlias)) {
                const auto index =
                    columnIndex(*baseMetadata, column.column);
                if (!index) return std::nullopt;
                return ColumnAccess{SourceKind::Base, 0, *index};
            }
            for (std::size_t joinIndex = 0;
                 joinIndex < joins.size();
                 ++joinIndex) {
                if (!matchesRelation(column.table,
                                     joins[joinIndex].table,
                                     joins[joinIndex].alias)) {
                    continue;
                }
                const auto index = columnIndex(
                    *joins[joinIndex].metadata, column.column);
                if (!index) return std::nullopt;
                return ColumnAccess{SourceKind::Join,
                                    joinIndex,
                                    *index};
            }
            return std::nullopt;
        }

        std::optional<ColumnAccess> result;
        if (const auto index =
                columnIndex(*baseMetadata, column.column)) {
            result = ColumnAccess{SourceKind::Base, 0, *index};
        }
        for (std::size_t joinIndex = 0;
             joinIndex < joins.size();
             ++joinIndex) {
            if (const auto index = columnIndex(
                    *joins[joinIndex].metadata, column.column)) {
                if (result) return std::nullopt;
                result = ColumnAccess{SourceKind::Join,
                                      joinIndex,
                                      *index};
            }
        }
        return result;
    };

    std::set<std::size_t> baseRequiredSet;
    auto requireColumn = [&](const ColumnAccess& column) {
        if (column.source == SourceKind::Base) {
            baseRequiredSet.insert(column.column);
        } else {
            joins[column.join].requiredSet.insert(column.column);
        }
    };
    for (const auto& join : joins) {
        baseRequiredSet.insert(join.baseKeyColumn);
    }

    std::vector<ColumnAccess> groupColumns;
    groupColumns.reserve(statement.groupBy.size());
    for (const auto& expression : statement.groupBy) {
        const auto* column =
            dynamic_cast<const ColumnRef*>(expression.get());
        if (!column) return std::nullopt;
        auto access = resolveColumn(*column);
        if (!access) return std::nullopt;
        requireColumn(*access);
        groupColumns.push_back(*access);
    }

    enum class AggregateKind { Count, Sum, Average, Max, Min, LoneWolf };
    struct AggregatePlan {
        AggregateKind kind = AggregateKind::Count;
        std::optional<ColumnAccess> argument;
    };
    struct ProjectionPlan {
        bool aggregate = false;
        std::optional<ColumnAccess> representative;
        AggregatePlan aggregatePlan;
    };

    std::vector<ProjectionPlan> projections;
    projections.reserve(statement.columns.size());
    bool hasAggregateProjection = false;
    for (const auto& expression : statement.columns) {
        if (const auto* function =
                dynamic_cast<const FunctionCall*>(expression.get())) {
            if (function->distinct ||
                !isSimpleAggregateName(function->name)) {
                return std::nullopt;
            }
            ProjectionPlan projection;
            projection.aggregate = true;
            hasAggregateProjection = true;
            if (function->name == "headcount") {
                projection.aggregatePlan.kind = AggregateKind::Count;
            } else if (function->name == "stack") {
                projection.aggregatePlan.kind = AggregateKind::Sum;
            } else if (function->name == "mid") {
                projection.aggregatePlan.kind = AggregateKind::Average;
            } else if (function->name == "goat") {
                projection.aggregatePlan.kind = AggregateKind::Max;
            } else if (function->name == "L") {
                projection.aggregatePlan.kind = AggregateKind::Min;
            } else if (isLoneWolfName(function->name)) {
                projection.aggregatePlan.kind = AggregateKind::LoneWolf;
            }

            const bool countWildcard =
                function->name == "headcount" &&
                (function->args.empty() ||
                 dynamic_cast<const Wildcard*>(
                     function->args.front().get()));
            if (!countWildcard) {
                if (function->args.size() != 1) return std::nullopt;
                const auto* argument =
                    dynamic_cast<const ColumnRef*>(
                        function->args.front().get());
                if (!argument) return std::nullopt;
                auto access = resolveColumn(*argument);
                if (!access) return std::nullopt;
                requireColumn(*access);
                projection.aggregatePlan.argument = *access;
            }
            projections.push_back(std::move(projection));
            continue;
        }

        const auto* column =
            dynamic_cast<const ColumnRef*>(expression.get());
        if (!column || hasAggregate(expression.get())) {
            return std::nullopt;
        }
        auto access = resolveColumn(*column);
        if (!access) return std::nullopt;
        bool groupedColumn = false;
        for (const auto& groupColumn : groupColumns) {
            if (sameColumn(*access, groupColumn)) {
                groupedColumn = true;
                break;
            }
        }
        if (!groupedColumn) return std::nullopt;
        requireColumn(*access);
        ProjectionPlan projection;
        projection.representative = *access;
        projections.push_back(std::move(projection));
    }
    if (!hasAggregateProjection) return std::nullopt;

    const std::size_t missing =
        std::numeric_limits<std::size_t>::max();
    const std::vector<std::size_t> baseRequiredColumns(
        baseRequiredSet.begin(), baseRequiredSet.end());
    std::vector<std::size_t> baseFieldPositions(
        baseMetadata->columns.size(), missing);
    for (std::size_t index = 0;
         index < baseRequiredColumns.size();
         ++index) {
        baseFieldPositions[baseRequiredColumns[index]] = index;
    }
    for (auto& join : joins) {
        join.requiredColumns.assign(
            join.requiredSet.begin(), join.requiredSet.end());
        join.fieldPositions.assign(
            join.metadata->columns.size(), missing);
        for (std::size_t index = 0;
             index < join.requiredColumns.size();
             ++index) {
            join.fieldPositions[join.requiredColumns[index]] = index;
        }
    }

    struct AggregateState {
        std::int64_t count = 0;
        double sum = 0.0;
        bool allIntegers = true;
        std::optional<Value> extremum;
        std::vector<double> samples;
    };
    struct GroupState {
        Tuple representatives;
        std::vector<AggregateState> aggregates;
    };

    std::unordered_map<Tuple, GroupState,
                       TupleKeyHash, TupleKeyEqual> groups;
    auto initializeGroup = [&](GroupState& group) {
        if (!group.representatives.empty() ||
            !group.aggregates.empty()) {
            return;
        }
        group.representatives.resize(
            projections.size(), Value::null());
        group.aggregates.resize(projections.size());
    };
    if (groupColumns.empty()) {
        initializeGroup(groups[{}]);
    }

    std::vector<RawField> baseFields;
    baseFields.reserve(baseRequiredColumns.size());
    std::vector<std::vector<RawField>> joinedFields(joins.size());
    for (std::size_t joinIndex = 0;
         joinIndex < joins.size();
         ++joinIndex) {
        joinedFields[joinIndex].reserve(
            joins[joinIndex].requiredColumns.size());
    }

    auto fieldAt =
        [&](const ColumnAccess& column) -> const RawField& {
        if (column.source == SourceKind::Base) {
            const auto position = baseFieldPositions[column.column];
            if (position == missing || position >= baseFields.size()) {
                throw std::runtime_error(
                    "Rowid-seek join base column was not decoded");
            }
            return baseFields[position];
        }
        const auto& join = joins[column.join];
        const auto position = join.fieldPositions[column.column];
        if (position == missing ||
            position >= joinedFields[column.join].size()) {
            throw std::runtime_error(
                "Rowid-seek join column was not decoded");
        }
        return joinedFields[column.join][position];
    };

    auto makeOutput = [&](GroupState& group) {
        initializeGroup(group);
        Tuple output;
        output.reserve(projections.size());
        for (std::size_t index = 0;
             index < projections.size();
             ++index) {
            const auto& projection = projections[index];
            if (!projection.aggregate) {
                output.push_back(group.representatives[index]);
                continue;
            }
            const auto& aggregate = projection.aggregatePlan;
            const auto& state = group.aggregates[index];
            if (aggregate.kind == AggregateKind::Count) {
                output.push_back(Value(state.count));
            } else if (aggregate.kind == AggregateKind::LoneWolf) {
                output.push_back(Value(countLoneWolves(state.samples)));
            } else if (state.count == 0) {
                output.push_back(Value::null());
            } else if (aggregate.kind == AggregateKind::Average) {
                output.push_back(Value(
                    state.sum / static_cast<double>(state.count)));
            } else if (aggregate.kind == AggregateKind::Sum) {
                output.push_back(state.allIntegers
                    ? Value(static_cast<std::int64_t>(state.sum))
                    : Value(state.sum));
            } else {
                output.push_back(
                    state.extremum.value_or(Value::null()));
            }
        }
        return output;
    };

    auto joinDomainProvesEmpty = [&]() {
        for (const auto& join : joins) {
            ++stats_.joinDomainFiltersChecked;
            const auto& baseRange = ensureColumnRange(
                statement.fromTable,
                baseMetadata->columns[join.baseKeyColumn].name);
            const auto& joinRange = ensureColumnRange(
                join.table,
                join.metadata->columns[join.primaryKeyColumn].name);
            if (!baseRange.present || !joinRange.present) return true;
            if (baseRange.max.compare(joinRange.min) < 0 ||
                baseRange.min.compare(joinRange.max) > 0) {
                return true;
            }
        }
        return false;
    };

    if (joinDomainProvesEmpty()) {
        ++stats_.joinDomainScansSkipped;
        stats_.joinDomainRowsSkipped +=
            tableStatistics_[statement.fromTable].rowCount;
        NativeQueryResult result;
        result.columns = outputNames(statement, nullptr);
        if (groupColumns.empty()) {
            result.rows.push_back(makeOutput(groups[{}]));
        }
        ++stats_.rowIdSeekJoinQueries;
        return result;
    }

    std::vector<MappedHeapFile*> joinMappedFiles;
    joinMappedFiles.reserve(joins.size());
    for (const auto& join : joins) {
        joinMappedFiles.push_back(&mappedHeap(join.table));
    }

    auto decodeJoinRow =
        [&](std::size_t joinIndex,
            RowId rowId) {
        auto& join = joins[joinIndex];
        joinedFields[joinIndex].clear();
        if (join.requiredColumns.empty()) return true;
        auto decode = [&](const std::uint8_t* data, std::size_t length) {
            decodeRawProjectedInto(
                data, length,
                join.requiredColumns,
                joinedFields[joinIndex]);
        };
        bool usedMapping = false;
        bool found = false;
        if (joinMappedFiles[joinIndex]->isMapped()) {
            found = joinMappedFiles[joinIndex]->readRawRowFast(
                rowId, decode);
            usedMapping = found;
        }
        if (!found) {
            auto file = heap(join.table);
            found = file.readRawRowFast(rowId, decode);
        }
        if (found) {
            ++stats_.rowsRead;
            ++stats_.rowCopiesAvoided;
            if (usedMapping) ++stats_.virtualMemoryRowIdReads;
            stats_.decodedColumns += join.requiredColumns.size();
            stats_.skippedColumns +=
                join.metadata->columns.size() -
                join.requiredColumns.size();
        }
        return found;
    };

    auto lookupJoin =
        [&](std::size_t joinIndex, const Value& key) {
        auto& join = joins[joinIndex];
        ++stats_.indexLookups;
        ++stats_.rowIdSeekJoinLookups;
        auto rowId = primaryIndex(join.table).find(key);
        if (!rowId) {
            ++stats_.rowIdSeekJoinMisses;
            return false;
        }
        if (decodeJoinRow(joinIndex, *rowId)) return true;

        primaryIndexes_.erase(join.table);
        ++stats_.indexLookups;
        ++stats_.rowIdSeekJoinLookups;
        rowId = primaryIndex(join.table).find(key);
        if (!rowId || !decodeJoinRow(joinIndex, *rowId)) {
            ++stats_.rowIdSeekJoinMisses;
            return false;
        }
        return true;
    };

    auto file = heap(statement.fromTable);
    auto& mappedFile = mappedHeap(statement.fromTable);
    ++stats_.tableScans;
    auto visitBaseRow =
        [&](RowId, const std::uint8_t* data, std::size_t length) {
        decodeRawProjectedInto(
            data, length, baseRequiredColumns, baseFields);
        ++stats_.rowsRead;
        ++stats_.rawRowsScanned;
        ++stats_.rowCopiesAvoided;
        ++stats_.rowIdSeekJoinBaseRows;
        stats_.decodedColumns += baseRequiredColumns.size();
        stats_.skippedColumns +=
            baseMetadata->columns.size() - baseRequiredColumns.size();

        for (std::size_t joinIndex = 0;
             joinIndex < joins.size();
             ++joinIndex) {
            const auto& join = joins[joinIndex];
            const auto position =
                baseFieldPositions[join.baseKeyColumn];
            if (position == missing || position >= baseFields.size()) {
                throw std::runtime_error(
                    "Rowid-seek join key was not decoded");
            }
            const auto& keyField = baseFields[position];
            if (keyField.isNull ||
                !lookupJoin(joinIndex, keyField.toValue())) {
                return;
            }
        }

        Tuple key;
        key.reserve(groupColumns.size());
        for (const auto& column : groupColumns) {
            key.push_back(fieldAt(column).toValue());
        }
        auto& group = groups[key];
        initializeGroup(group);
        for (std::size_t index = 0;
             index < projections.size();
             ++index) {
            const auto& projection = projections[index];
            if (!projection.aggregate) {
                if (group.representatives[index].isNull()) {
                    group.representatives[index] =
                        fieldAt(*projection.representative).toValue();
                }
                continue;
            }

            auto& state = group.aggregates[index];
            const auto& aggregate = projection.aggregatePlan;
            if (!aggregate.argument) {
                ++state.count;
                continue;
            }
            const auto& field = fieldAt(*aggregate.argument);
            if (field.isNull) continue;
            if (aggregate.kind == AggregateKind::Count) {
                ++state.count;
                continue;
            }
            if (aggregate.kind == AggregateKind::Sum ||
                aggregate.kind == AggregateKind::Average ||
                aggregate.kind == AggregateKind::LoneWolf) {
                if (!field.numeric()) {
                    throw std::runtime_error(
                        "Numeric aggregate on non-number");
                }
                ++state.count;
                const double numeric = field.asReal();
                state.sum += numeric;
                state.allIntegers =
                    state.allIntegers &&
                    field.type == ValueType::Integer;
                if (aggregate.kind == AggregateKind::LoneWolf) {
                    state.samples.push_back(numeric);
                }
                continue;
            }

            Value value = field.toValue();
            ++state.count;
            if (!state.extremum) {
                state.extremum = std::move(value);
                continue;
            }
            const int comparison = value.compare(*state.extremum);
            if ((aggregate.kind == AggregateKind::Max &&
                 comparison > 0) ||
                (aggregate.kind == AggregateKind::Min &&
                 comparison < 0)) {
                state.extremum = std::move(value);
            }
        }
    };
    if (mappedFile.isMapped()) {
        ++stats_.virtualMemoryScanQueries;
        mappedFile.scanRawRowsFast(
            [&](RowId rowId,
                const std::uint8_t* data,
                std::size_t length) {
            ++stats_.virtualMemoryRowsScanned;
            visitBaseRow(rowId, data, length);
        });
    } else {
        file.scanRawRowsFast(visitBaseRow);
    }

    NativeQueryResult result;
    result.columns = outputNames(statement, nullptr);
    for (auto& item : groups) {
        result.rows.push_back(makeOutput(item.second));
    }
    ++stats_.rowIdSeekJoinQueries;
    return result;
}

std::optional<std::vector<NativeEngine::EvalRow>>
NativeEngine::executeCostBasedJoins(const SelectStmt& statement) {
    if (statement.joins.size() < 2 ||
        statement.joins.size() >= sizeof(std::size_t) * 8) {
        return std::nullopt;
    }
    for (const auto& join : statement.joins) {
        if (join.type != JoinType::INNER) return std::nullopt;
    }
    for (const auto& expression : statement.columns) {
        if (dynamic_cast<const Wildcard*>(expression.get())) {
            return std::nullopt;
        }
    }

    struct JoinPredicate {
        std::size_t leftRelation = 0;
        std::size_t rightRelation = 0;
        const ColumnRef* left = nullptr;
        const ColumnRef* right = nullptr;
        const ASTNode* expression = nullptr;
    };

    std::vector<RelationData> relations;
    relations.reserve(statement.joins.size() + 1);
    relations.push_back(
        {statement.fromTable, statement.fromAlias, {}, {}});
    for (const auto& join : statement.joins) {
        relations.push_back({join.table, join.alias, {}, {}});
    }

    auto relationFor = [&](const ColumnRef& column)
        -> std::optional<std::size_t> {
        if (column.table.empty()) return std::nullopt;
        for (std::size_t index = 0; index < relations.size(); ++index) {
            if (column.table == relations[index].table ||
                (!relations[index].alias.empty() &&
                 column.table == relations[index].alias)) {
                return index;
            }
        }
        return std::nullopt;
    };

    std::vector<JoinPredicate> predicates;
    predicates.reserve(statement.joins.size());
    for (const auto& join : statement.joins) {
        auto* equality =
            dynamic_cast<const BinaryOp*>(join.condition.get());
        if (!equality || equality->op != "=") return std::nullopt;
        auto* left =
            dynamic_cast<const ColumnRef*>(equality->left.get());
        auto* right =
            dynamic_cast<const ColumnRef*>(equality->right.get());
        if (!left || !right) return std::nullopt;
        auto leftRelation = relationFor(*left);
        auto rightRelation = relationFor(*right);
        if (!leftRelation || !rightRelation ||
            *leftRelation == *rightRelation) {
            return std::nullopt;
        }
        predicates.push_back(
            {*leftRelation, *rightRelation, left, right,
             join.condition.get()});
    }

    for (std::size_t relationIndex = 0;
         relationIndex < relations.size();
         ++relationIndex) {
        auto& relation = relations[relationIndex];
        auto scan = scanTable(relation.table, relation.alias);
        relation.rows.reserve(scan.size());
        for (auto& row : scan) {
            relation.rows.push_back(std::move(row.row));
        }
        const auto* metadata = catalog_.getTable(relation.table);
        if (!metadata) {
            throw std::runtime_error(
                "Unknown table: " + relation.table);
        }
        auto& cached = tableStatistics_[relation.table];
        if (cached.rowCount != relation.rows.size()) {
            cached = {};
            cached.rowCount = relation.rows.size();
        }
        std::unordered_set<std::string> requiredColumns;
        for (const auto& predicate : predicates) {
            if (predicate.leftRelation == relationIndex) {
                requiredColumns.insert(predicate.left->column);
            }
            if (predicate.rightRelation == relationIndex) {
                requiredColumns.insert(predicate.right->column);
            }
        }
        for (const auto& columnName : requiredColumns) {
            auto foundCached =
                cached.distinctCounts.find(columnName);
            if (foundCached != cached.distinctCounts.end()) {
                relation.distinctCounts[columnName] =
                    foundCached->second;
                continue;
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
            std::unordered_set<Value, ValueHash> distinct;
            for (const auto& row : relation.rows) {
                if (!row.values[column].isNull()) {
                    distinct.insert(row.values[column]);
                }
            }
            const auto count =
                std::max<std::size_t>(1, distinct.size());
            cached.distinctCounts[columnName] = count;
            relation.distinctCounts[columnName] = count;
        }
    }

    struct Plan {
        bool valid = false;
        double cost = 0.0;
        double rows = 0.0;
        std::vector<std::size_t> order;
    };
    const std::size_t relationCount = relations.size();
    const std::size_t stateCount =
        static_cast<std::size_t>(1) << relationCount;
    std::vector<Plan> plans(stateCount);
    for (std::size_t relation = 0;
         relation < relationCount;
         ++relation) {
        auto& plan = plans[static_cast<std::size_t>(1) << relation];
        plan.valid = true;
        plan.rows = static_cast<double>(
            relations[relation].rows.size());
        plan.cost = plan.rows;
        plan.order.push_back(relation);
    }

    auto distinctCount = [&](std::size_t relation,
                             const ColumnRef& column) {
        const auto found =
            relations[relation].distinctCounts.find(column.column);
        return static_cast<double>(
            found == relations[relation].distinctCounts.end()
                ? std::max<std::size_t>(
                      1, relations[relation].rows.size())
                : found->second);
    };

    for (std::size_t mask = 1; mask < stateCount; ++mask) {
        if (!plans[mask].valid) continue;
        for (std::size_t next = 0;
             next < relationCount;
             ++next) {
            const std::size_t bit =
                static_cast<std::size_t>(1) << next;
            if ((mask & bit) != 0) continue;
            double selectivity = 1.0;
            bool connected = false;
            for (const auto& predicate : predicates) {
                const bool leftIn =
                    (mask & (static_cast<std::size_t>(1)
                             << predicate.leftRelation)) != 0;
                const bool rightIn =
                    (mask & (static_cast<std::size_t>(1)
                             << predicate.rightRelation)) != 0;
                if (predicate.leftRelation == next && rightIn) {
                    connected = true;
                    selectivity *= 1.0 / std::max(
                        distinctCount(next, *predicate.left),
                        distinctCount(predicate.rightRelation,
                                      *predicate.right));
                } else if (predicate.rightRelation == next && leftIn) {
                    connected = true;
                    selectivity *= 1.0 / std::max(
                        distinctCount(predicate.leftRelation,
                                      *predicate.left),
                        distinctCount(next, *predicate.right));
                }
            }
            if (!connected) continue;
            const double nextRows = static_cast<double>(
                relations[next].rows.size());
            const double outputRows = std::max(
                plans[mask].rows * nextRows * selectivity,
                (plans[mask].rows == 0.0 || nextRows == 0.0)
                    ? 0.0 : 1.0);
            const double cost = plans[mask].cost +
                                plans[mask].rows +
                                nextRows + outputRows;
            const std::size_t combined = mask | bit;
            ++stats_.joinPlansEnumerated;
            if (!plans[combined].valid ||
                cost < plans[combined].cost) {
                plans[combined] = plans[mask];
                plans[combined].valid = true;
                plans[combined].cost = cost;
                plans[combined].rows = outputRows;
                plans[combined].order.push_back(next);
            }
        }
    }

    const auto& best = plans[stateCount - 1];
    if (!best.valid) return std::nullopt;
    stats_.estimatedJoinCost = best.cost;
    bool changed = false;
    for (std::size_t index = 0; index < best.order.size(); ++index) {
        if (best.order[index] != index) {
            changed = true;
            break;
        }
    }
    if (changed) ++stats_.joinOrderChanges;

    std::vector<EvalRow> rows =
        std::move(relations[best.order.front()].rows);
    std::size_t joinedMask =
        static_cast<std::size_t>(1) << best.order.front();
    for (std::size_t orderIndex = 1;
         orderIndex < best.order.size();
         ++orderIndex) {
        const std::size_t next = best.order[orderIndex];
        const JoinPredicate* keyPredicate = nullptr;
        bool nextIsLeft = false;
        for (const auto& predicate : predicates) {
            const bool leftJoined =
                (joinedMask & (static_cast<std::size_t>(1)
                               << predicate.leftRelation)) != 0;
            const bool rightJoined =
                (joinedMask & (static_cast<std::size_t>(1)
                               << predicate.rightRelation)) != 0;
            if (predicate.leftRelation == next && rightJoined) {
                keyPredicate = &predicate;
                nextIsLeft = true;
                break;
            }
            if (predicate.rightRelation == next && leftJoined) {
                keyPredicate = &predicate;
                nextIsLeft = false;
                break;
            }
        }
        if (!keyPredicate) return std::nullopt;
        const ColumnRef& nextKey = nextIsLeft
            ? *keyPredicate->left : *keyPredicate->right;
        const ColumnRef& joinedKey = nextIsLeft
            ? *keyPredicate->right : *keyPredicate->left;
        auto& nextRows = relations[next].rows;
        std::vector<EvalRow> joined;
        const auto joinedSchema = combineSchemas(
            rows.empty()
                ? boundSchema(relations[best.order.front()].table,
                              relations[best.order.front()].alias)
                : rows.front().columns,
            nextRows.empty()
                ? boundSchema(relations[next].table,
                              relations[next].alias)
                : nextRows.front().columns);

        if (rows.size() <= nextRows.size()) {
            std::unordered_multimap<Value, const EvalRow*, ValueHash>
                index;
            index.reserve(rows.size());
            BloomFilter bloom(rows.size());
            ++stats_.bloomFilterBuilds;
            for (const auto& row : rows) {
                Value key = resolveColumn(joinedKey, row);
                if (!key.isNull()) {
                    bloom.add(key);
                    index.emplace(std::move(key), &row);
                }
            }
            for (const auto& nextRow : nextRows) {
                Value key = resolveColumn(nextKey, nextRow);
                if (key.isNull()) continue;
                ++stats_.bloomFilterChecks;
                if (!bloom.mayContain(key)) {
                    ++stats_.bloomFilterRejects;
                    continue;
                }
                ++stats_.hashJoinProbes;
                const auto range = index.equal_range(key);
                for (auto match = range.first;
                     match != range.second;
                     ++match) {
                    EvalRow combined = *match->second;
                    combined.columns = joinedSchema;
                    combined.values.insert(
                        combined.values.end(),
                        nextRow.values.begin(),
                        nextRow.values.end());
                    joined.push_back(std::move(combined));
                }
            }
        } else {
            std::unordered_multimap<Value, const EvalRow*, ValueHash>
                index;
            index.reserve(nextRows.size());
            BloomFilter bloom(nextRows.size());
            ++stats_.bloomFilterBuilds;
            for (const auto& row : nextRows) {
                Value key = resolveColumn(nextKey, row);
                if (!key.isNull()) {
                    bloom.add(key);
                    index.emplace(std::move(key), &row);
                }
            }
            for (const auto& row : rows) {
                Value key = resolveColumn(joinedKey, row);
                if (key.isNull()) continue;
                ++stats_.bloomFilterChecks;
                if (!bloom.mayContain(key)) {
                    ++stats_.bloomFilterRejects;
                    continue;
                }
                ++stats_.hashJoinProbes;
                const auto range = index.equal_range(key);
                for (auto match = range.first;
                     match != range.second;
                     ++match) {
                    EvalRow combined = row;
                    combined.columns = joinedSchema;
                    combined.values.insert(
                        combined.values.end(),
                        match->second->values.begin(),
                        match->second->values.end());
                    joined.push_back(std::move(combined));
                }
            }
        }

        joinedMask |= static_cast<std::size_t>(1) << next;
        joined.erase(std::remove_if(
            joined.begin(), joined.end(),
            [&](const EvalRow& row) {
                for (const auto& predicate : predicates) {
                    const std::size_t leftBit =
                        static_cast<std::size_t>(1)
                        << predicate.leftRelation;
                    const std::size_t rightBit =
                        static_cast<std::size_t>(1)
                        << predicate.rightRelation;
                    if ((joinedMask & leftBit) != 0 &&
                        (joinedMask & rightBit) != 0 &&
                        !eval(predicate.expression, row).asBoolean()) {
                        return true;
                    }
                }
                return false;
            }), joined.end());
        rows = std::move(joined);
    }
    return rows;
}

std::optional<NativeQueryResult> NativeEngine::executeDirectAggregateSelect(
    const SelectStmt& statement) {
    if (!statement.joins.empty() || statement.distinct ||
        statement.having || !statement.orderBy.empty() ||
        statement.limit || statement.offset ||
        !selectHasAggregate(statement)) {
        return std::nullopt;
    }

    const auto* metadata = catalog_.getTable(statement.fromTable);
    if (!metadata) {
        throw std::runtime_error(
            "Unknown table: " + statement.fromTable);
    }

    auto resolveColumnIndex = [&](const ColumnRef& column) {
        if (!column.table.empty() &&
            column.table != statement.fromTable &&
            column.table != statement.fromAlias) {
            throw std::runtime_error(
                "Unknown table qualifier: " + column.table);
        }
        std::optional<std::size_t> result;
        for (std::size_t index = 0;
             index < metadata->columns.size();
             ++index) {
            if (metadata->columns[index].name != column.column) {
                continue;
            }
            if (result && column.table.empty()) {
                throw std::runtime_error(
                    "Ambiguous column: " + column.column);
            }
            result = index;
        }
        if (!result) {
            throw std::runtime_error("Unknown column: " +
                (column.table.empty() ? "" : column.table + ".") +
                column.column);
        }
        return *result;
    };

    struct PredicatePlan {
        std::size_t column = 0;
        std::string op;
        Value literal;
        bool columnOnLeft = true;
    };
    std::optional<PredicatePlan> predicate;
    auto comparisonOperator = [](const std::string& op) {
        return op == "=" || op == "!=" || op == "<" || op == ">" ||
               op == "<=" || op == ">=";
    };
    if (statement.where) {
        const auto* binary =
            dynamic_cast<const BinaryOp*>(statement.where.get());
        if (!binary || !comparisonOperator(binary->op)) {
            return std::nullopt;
        }
        const auto* leftColumn =
            dynamic_cast<const ColumnRef*>(binary->left.get());
        const auto* rightColumn =
            dynamic_cast<const ColumnRef*>(binary->right.get());
        const auto* leftLiteral =
            dynamic_cast<const Literal*>(binary->left.get());
        const auto* rightLiteral =
            dynamic_cast<const Literal*>(binary->right.get());
        if (leftColumn && rightLiteral) {
            predicate = PredicatePlan{
                resolveColumnIndex(*leftColumn),
                binary->op,
                literalValue(*rightLiteral),
                true};
        } else if (rightColumn && leftLiteral) {
            predicate = PredicatePlan{
                resolveColumnIndex(*rightColumn),
                binary->op,
                literalValue(*leftLiteral),
                false};
        } else {
            return std::nullopt;
        }
    }

    enum class AggregateKind { Count, Sum, Average, Max, Min, LoneWolf };
    struct AggregatePlan {
        AggregateKind kind = AggregateKind::Count;
        std::optional<std::size_t> argumentColumn;
    };
    struct ProjectionPlan {
        bool aggregate = false;
        std::optional<std::size_t> representativeColumn;
        AggregatePlan aggregatePlan;
    };

    std::set<std::size_t> requiredSet;
    auto require = [&](std::size_t column) {
        requiredSet.insert(column);
    };
    if (predicate) require(predicate->column);

    std::vector<std::size_t> groupColumns;
    groupColumns.reserve(statement.groupBy.size());
    for (const auto& expression : statement.groupBy) {
        const auto* column =
            dynamic_cast<const ColumnRef*>(expression.get());
        if (!column) return std::nullopt;
        const auto index = resolveColumnIndex(*column);
        groupColumns.push_back(index);
        require(index);
    }

    std::vector<ProjectionPlan> projections;
    projections.reserve(statement.columns.size());
    bool hasAggregateProjection = false;
    for (const auto& expression : statement.columns) {
        if (auto* function =
                dynamic_cast<const FunctionCall*>(expression.get())) {
            if (function->distinct) return std::nullopt;
            ProjectionPlan plan;
            plan.aggregate = true;
            hasAggregateProjection = true;
            if (function->name == "headcount") {
                plan.aggregatePlan.kind = AggregateKind::Count;
            } else if (function->name == "stack") {
                plan.aggregatePlan.kind = AggregateKind::Sum;
            } else if (function->name == "mid") {
                plan.aggregatePlan.kind = AggregateKind::Average;
            } else if (function->name == "goat") {
                plan.aggregatePlan.kind = AggregateKind::Max;
            } else if (function->name == "L") {
                plan.aggregatePlan.kind = AggregateKind::Min;
            } else if (isLoneWolfName(function->name)) {
                plan.aggregatePlan.kind = AggregateKind::LoneWolf;
            } else {
                return std::nullopt;
            }

            const bool countWildcard =
                function->name == "headcount" &&
                (function->args.empty() ||
                 dynamic_cast<const Wildcard*>(
                     function->args.front().get()));
            if (!countWildcard) {
                if (function->args.size() != 1) return std::nullopt;
                const auto* argument =
                    dynamic_cast<const ColumnRef*>(
                        function->args.front().get());
                if (!argument) return std::nullopt;
                const auto index = resolveColumnIndex(*argument);
                plan.aggregatePlan.argumentColumn = index;
                require(index);
            }
            projections.push_back(std::move(plan));
            continue;
        }

        const auto* column =
            dynamic_cast<const ColumnRef*>(expression.get());
        if (!column) return std::nullopt;
        const auto index = resolveColumnIndex(*column);
        if (std::find(groupColumns.begin(),
                      groupColumns.end(),
                      index) == groupColumns.end()) {
            return std::nullopt;
        }
        ProjectionPlan plan;
        plan.representativeColumn = index;
        require(index);
        projections.push_back(std::move(plan));
    }
    if (!hasAggregateProjection) return std::nullopt;

    const std::vector<std::size_t> requiredColumns(
        requiredSet.begin(), requiredSet.end());
    const std::size_t missing =
        std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> fieldPositions(
        metadata->columns.size(), missing);
    for (std::size_t index = 0;
         index < requiredColumns.size();
         ++index) {
        fieldPositions[requiredColumns[index]] = index;
    }

    struct AggregateState {
        std::int64_t count = 0;
        double sum = 0.0;
        bool allIntegers = true;
        std::optional<Value> extremum;
        std::vector<double> samples;
    };
    struct GroupState {
        Tuple representatives;
        std::vector<AggregateState> aggregates;
    };

    const bool scalarAggregate = groupColumns.empty();
    GroupState scalarGroup;
    std::unordered_map<Tuple, GroupState,
                       TupleKeyHash, TupleKeyEqual> groups;
    auto initializeGroup = [&](GroupState& group) {
        if (!group.representatives.empty() ||
            !group.aggregates.empty()) {
            return;
        }
        group.representatives.resize(
            projections.size(), Value::null());
        group.aggregates.resize(projections.size());
    };
    if (scalarAggregate) initializeGroup(scalarGroup);

    auto makeOutput = [&](GroupState& group) {
        initializeGroup(group);
        Tuple output;
        output.reserve(projections.size());
        for (std::size_t index = 0;
             index < projections.size();
             ++index) {
            const auto& projection = projections[index];
            if (!projection.aggregate) {
                output.push_back(group.representatives[index]);
                continue;
            }
            const auto& aggregate = projection.aggregatePlan;
            const auto& state = group.aggregates[index];
            if (aggregate.kind == AggregateKind::Count) {
                output.push_back(Value(state.count));
            } else if (aggregate.kind == AggregateKind::LoneWolf) {
                output.push_back(Value(countLoneWolves(state.samples)));
            } else if (state.count == 0) {
                output.push_back(Value::null());
            } else if (aggregate.kind == AggregateKind::Average) {
                output.push_back(Value(
                    state.sum / static_cast<double>(state.count)));
            } else if (aggregate.kind == AggregateKind::Sum) {
                output.push_back(state.allIntegers
                    ? Value(static_cast<std::int64_t>(state.sum))
                    : Value(state.sum));
            } else {
                output.push_back(
                    state.extremum.value_or(Value::null()));
            }
        }
        return output;
    };

    auto minMaxProvesEmpty =
        [&](const PredicatePlan& plan) {
            ++stats_.minMaxFiltersChecked;
            const auto& range = ensureColumnRange(
                statement.fromTable,
                metadata->columns[plan.column].name);
            if (plan.literal.isNull() || !range.present) return true;
            const auto op =
                normalizeColumnPredicateOp(plan.op, plan.columnOnLeft);
            const int literalVsMin = plan.literal.compare(range.min);
            const int literalVsMax = plan.literal.compare(range.max);
            if (op == "=") {
                return literalVsMin < 0 || literalVsMax > 0;
            }
            if (op == "!=") {
                return range.min.compare(plan.literal) == 0 &&
                       range.max.compare(plan.literal) == 0;
            }
            if (op == "<") {
                return range.min.compare(plan.literal) >= 0;
            }
            if (op == "<=") {
                return range.min.compare(plan.literal) > 0;
            }
            if (op == ">") {
                return range.max.compare(plan.literal) <= 0;
            }
            if (op == ">=") {
                return range.max.compare(plan.literal) < 0;
            }
            return false;
        };

    if (predicate && minMaxProvesEmpty(*predicate)) {
        ++stats_.minMaxScansSkipped;
        stats_.minMaxRowsSkipped += tableStatistics_[
            statement.fromTable].rowCount;
        NativeQueryResult result;
        result.columns = outputNames(statement, nullptr);
        if (scalarAggregate) {
            result.rows.push_back(makeOutput(scalarGroup));
        }
        ++stats_.directAggregateQueries;
        return result;
    }

    auto fieldAt = [&](const std::vector<RawField>& fields,
                       std::size_t column) -> const RawField& {
        const auto position = fieldPositions[column];
        if (position == missing || position >= fields.size()) {
            throw std::runtime_error(
                "Direct aggregate column was not decoded");
        }
        return fields[position];
    };

    auto predicateMatches = [&](const std::vector<RawField>& fields) {
        if (!predicate) return true;
        const auto& field = fieldAt(fields, predicate->column);
        if (field.isNull || predicate->literal.isNull()) {
            return false;
        }
        int order = compareRawToValue(field, predicate->literal);
        if (!predicate->columnOnLeft) order = -order;
        return comparisonMatches(predicate->op, order);
    };

    if (scalarAggregate && projections.size() == 1 &&
        projections.front().aggregate &&
        projections.front().aggregatePlan.kind ==
            AggregateKind::Count) {
        const auto argumentColumn =
            projections.front().aggregatePlan.argumentColumn;
        std::int64_t count = 0;
        auto file = heap(statement.fromTable);
        ++stats_.tableScans;
        file.scanRawRowsFast(
            [&](RowId, const std::uint8_t* data, std::size_t length) {
            ++stats_.rowsRead;
            ++stats_.rawRowsScanned;
            ++stats_.rowCopiesAvoided;
            stats_.decodedColumns += requiredColumns.size();
            stats_.skippedColumns +=
                metadata->columns.size() - requiredColumns.size();

            RawField predicateField;
            if (predicate) {
                predicateField =
                    decodeRawColumn(data, length, predicate->column);
                if (predicateField.isNull ||
                    predicate->literal.isNull()) {
                    return;
                }
                int order =
                    compareRawToValue(predicateField,
                                      predicate->literal);
                if (!predicate->columnOnLeft) order = -order;
                if (!comparisonMatches(predicate->op, order)) return;
            }

            if (!argumentColumn) {
                ++count;
                return;
            }
            RawField argumentField =
                (predicate && *argumentColumn == predicate->column)
                    ? predicateField
                    : decodeRawColumn(data, length, *argumentColumn);
            if (!argumentField.isNull) ++count;
        });

        NativeQueryResult result;
        result.columns = outputNames(statement, nullptr);
        result.rows.push_back(Tuple{Value(count)});
        ++stats_.directAggregateQueries;
        return result;
    }

    auto file = heap(statement.fromTable);
    ++stats_.tableScans;
    std::vector<RawField> fields;
    fields.reserve(requiredColumns.size());
    file.scanRawRowsFast(
        [&](RowId, const std::uint8_t* data, std::size_t length) {
        decodeRawProjectedInto(data, length, requiredColumns, fields);
        ++stats_.rowsRead;
        ++stats_.rawRowsScanned;
        ++stats_.rowCopiesAvoided;
        stats_.decodedColumns += requiredColumns.size();
        stats_.skippedColumns +=
            metadata->columns.size() - requiredColumns.size();
        if (!predicateMatches(fields)) return;

        GroupState* targetGroup = &scalarGroup;
        if (!scalarAggregate) {
            Tuple key;
            key.reserve(groupColumns.size());
            for (const auto column : groupColumns) {
                key.push_back(fieldAt(fields, column).toValue());
            }
            targetGroup = &groups[key];
        }
        auto& group = *targetGroup;
        initializeGroup(group);
        for (std::size_t index = 0;
             index < projections.size();
             ++index) {
            const auto& projection = projections[index];
            if (!projection.aggregate) {
                if (group.representatives[index].isNull()) {
                    group.representatives[index] =
                        fieldAt(fields,
                                *projection.representativeColumn)
                            .toValue();
                }
                continue;
            }

            auto& state = group.aggregates[index];
            const auto& aggregate = projection.aggregatePlan;
            if (!aggregate.argumentColumn) {
                ++state.count;
                continue;
            }
            const auto& field =
                fieldAt(fields, *aggregate.argumentColumn);
            if (field.isNull) continue;
            if (aggregate.kind == AggregateKind::Count) {
                ++state.count;
                continue;
            }
            if (aggregate.kind == AggregateKind::Sum ||
                aggregate.kind == AggregateKind::Average ||
                aggregate.kind == AggregateKind::LoneWolf) {
                if (!field.numeric()) {
                    throw std::runtime_error(
                        "Numeric aggregate on non-number");
                }
                ++state.count;
                const double numeric = field.asReal();
                state.sum += numeric;
                state.allIntegers =
                    state.allIntegers &&
                    field.type == ValueType::Integer;
                if (aggregate.kind == AggregateKind::LoneWolf) {
                    state.samples.push_back(numeric);
                }
                continue;
            }

            Value value = field.toValue();
            ++state.count;
            if (!state.extremum) {
                state.extremum = std::move(value);
                continue;
            }
            const int comparison =
                value.compare(*state.extremum);
            if ((aggregate.kind == AggregateKind::Max &&
                 comparison > 0) ||
                (aggregate.kind == AggregateKind::Min &&
                 comparison < 0)) {
                state.extremum = std::move(value);
            }
        }
    });

    NativeQueryResult result;
    result.columns = outputNames(statement, nullptr);
    if (scalarAggregate) {
        result.rows.push_back(makeOutput(scalarGroup));
    } else {
        for (auto& item : groups) {
            result.rows.push_back(makeOutput(item.second));
        }
    }
    ++stats_.directAggregateQueries;
    return result;
}

bool NativeEngine::canVectorize(const SelectStmt& statement) const {
    if (!statement.joins.empty() || statement.distinct ||
        !statement.orderBy.empty() || statement.limit || statement.offset ||
        statement.having) {
        return false;
    }

    std::function<bool(const ASTNode*, bool)> supported =
        [&](const ASTNode* expression, bool allowAggregate) {
            if (!expression) return true;
            if (dynamic_cast<const Literal*>(expression) ||
                dynamic_cast<const ColumnRef*>(expression) ||
                dynamic_cast<const Wildcard*>(expression)) {
                return true;
            }
            if (auto* binary =
                    dynamic_cast<const BinaryOp*>(expression)) {
                return supported(binary->left.get(), false) &&
                       supported(binary->right.get(), false);
            }
            if (auto* unary =
                    dynamic_cast<const UnaryOp*>(expression)) {
                return supported(unary->operand.get(), false);
            }
            if (auto* function =
                    dynamic_cast<const FunctionCall*>(expression)) {
                if (!allowAggregate ||
                    (function->name != "headcount" &&
                     function->name != "stack" &&
                     function->name != "mid" &&
                     function->name != "goat" &&
                     function->name != "L")) {
                    return false;
                }
                if (function->args.empty()) {
                    return function->name == "headcount";
                }
                if (dynamic_cast<const Wildcard*>(
                        function->args.front().get())) {
                    return function->name == "headcount";
                }
                return supported(function->args.front().get(), false);
            }
            return false;
        };

    if (!supported(statement.where.get(), false)) return false;
    for (const auto& expression : statement.groupBy) {
        if (!supported(expression.get(), false)) return false;
    }
    for (const auto& expression : statement.columns) {
        if (!supported(expression.get(), true)) return false;
        if (hasAggregate(expression.get()) &&
            !dynamic_cast<const FunctionCall*>(expression.get())) {
            return false;
        }
    }
    return true;
}

std::size_t NativeEngine::resolveVectorColumn(
    const ColumnRef& column,
    const VectorBatch& batch) const {
    std::optional<std::size_t> result;
    for (std::size_t index = 0; index < batch.columns.size(); ++index) {
        const auto& candidate = batch.columns[index];
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
        result = index;
    }
    if (!result) {
        throw std::runtime_error("Unknown column: " +
            (column.table.empty() ? "" : column.table + ".") +
            column.column);
    }
    return *result;
}

NativeEngine::VectorPtr NativeEngine::evalVector(
    const ASTNode* expression,
    const VectorBatch& batch) const {
    if (!expression) {
        auto result = std::make_shared<TypedVector>();
        result->reserve(batch.rowCount);
        for (std::size_t row = 0; row < batch.rowCount; ++row) {
            result->append(Value::null());
        }
        return result;
    }
    if (auto* column = dynamic_cast<const ColumnRef*>(expression)) {
        return batch.values[resolveVectorColumn(*column, batch)];
    }
    if (auto* literal = dynamic_cast<const Literal*>(expression)) {
        Value value;
        switch (literal->kind) {
            case LiteralKind::INT:
                value = Value(static_cast<std::int64_t>(literal->ival));
                break;
            case LiteralKind::FLOAT:
                value = Value(literal->fval);
                break;
            case LiteralKind::STRING:
                value = Value(literal->sval);
                break;
            case LiteralKind::NUL:
                value = Value::null();
                break;
            case LiteralKind::BOOL:
                value = Value(literal->bval);
                break;
        }
        auto result = std::make_shared<TypedVector>(value.type());
        result->reserve(batch.rowCount);
        for (std::size_t row = 0; row < batch.rowCount; ++row) {
            result->append(value);
        }
        return result;
    }
    if (auto* binary = dynamic_cast<const BinaryOp*>(expression)) {
        const auto* leftColumn =
            dynamic_cast<const ColumnRef*>(binary->left.get());
        const auto* rightColumn =
            dynamic_cast<const ColumnRef*>(binary->right.get());
        const auto* leftLiteral =
            dynamic_cast<const Literal*>(binary->left.get());
        const auto* rightLiteral =
            dynamic_cast<const Literal*>(binary->right.get());
        const bool comparison =
            binary->op == "=" || binary->op == "!=" ||
            binary->op == "<" || binary->op == ">" ||
            binary->op == "<=" || binary->op == ">=";
        if (comparison &&
            ((leftColumn && rightLiteral) ||
             (rightColumn && leftLiteral))) {
            const bool columnOnLeft = leftColumn != nullptr;
            const auto* column =
                columnOnLeft ? leftColumn : rightColumn;
            const auto* literal =
                columnOnLeft ? rightLiteral : leftLiteral;
            auto values = batch.values[
                resolveVectorColumn(*column, batch)];
            Value scalar;
            switch (literal->kind) {
                case LiteralKind::INT:
                    scalar = Value(
                        static_cast<std::int64_t>(literal->ival));
                    break;
                case LiteralKind::FLOAT:
                    scalar = Value(literal->fval);
                    break;
                case LiteralKind::STRING:
                    scalar = Value(literal->sval);
                    break;
                case LiteralKind::NUL:
                    scalar = Value::null();
                    break;
                case LiteralKind::BOOL:
                    scalar = Value(literal->bval);
                    break;
            }
            auto result = std::make_shared<TypedVector>(
                ValueType::Boolean);
            result->reserve(batch.rowCount);
            const bool numeric =
                values->type != ValueType::Text &&
                values->type != ValueType::Blob &&
                values->type != ValueType::Null &&
                scalar.isNumeric();
            for (std::size_t row = 0;
                 row < batch.rowCount;
                 ++row) {
                if (values->isNull(row) || scalar.isNull()) {
                    result->append(Value::null());
                    continue;
                }
                int order = 0;
                if (numeric) {
                    double current = 0.0;
                    if (values->type == ValueType::Integer) {
                        current = static_cast<double>(
                            std::get<std::vector<std::int64_t>>(
                                values->storage)[row]);
                    } else if (values->type == ValueType::Real) {
                        current = std::get<std::vector<double>>(
                            values->storage)[row];
                    } else {
                        current =
                            std::get<std::vector<std::uint8_t>>(
                                values->storage)[row] != 0
                            ? 1.0 : 0.0;
                    }
                    const double constant = scalar.asReal();
                    order = current < constant ? -1 :
                            (current > constant ? 1 : 0);
                } else {
                    order = values->value(row).compare(scalar);
                }
                if (!columnOnLeft) order = -order;
                bool matches = false;
                if (binary->op == "=") matches = order == 0;
                else if (binary->op == "!=") matches = order != 0;
                else if (binary->op == "<") matches = order < 0;
                else if (binary->op == ">") matches = order > 0;
                else if (binary->op == "<=") matches = order <= 0;
                else matches = order >= 0;
                result->append(Value(matches));
            }
            return result;
        }
        auto left = evalVector(binary->left.get(), batch);
        auto right = evalVector(binary->right.get(), batch);
        auto result = std::make_shared<TypedVector>();
        result->reserve(batch.rowCount);
        for (std::size_t row = 0; row < batch.rowCount; ++row) {
            result->append(applyBinary(
                binary->op, left->value(row), right->value(row)));
        }
        return result;
    }
    if (auto* unary = dynamic_cast<const UnaryOp*>(expression)) {
        auto values = evalVector(unary->operand.get(), batch);
        auto result = std::make_shared<TypedVector>();
        result->reserve(batch.rowCount);
        for (std::size_t row = 0; row < batch.rowCount; ++row) {
            result->append(
                applyUnary(unary->op, values->value(row)));
        }
        return result;
    }
    throw std::runtime_error("Unsupported vector expression");
}

ValueType NativeEngine::declaredValueType(const ColumnMeta& column) {
    const auto type = upper(column.type);
    if (type == "INTEGER" || type == "INT") {
        return ValueType::Integer;
    }
    if (type == "REAL" || type == "FLOAT" || type == "DOUBLE") {
        return ValueType::Real;
    }
    if (type == "TEXT" || type == "VARCHAR" || type == "STRING") {
        return ValueType::Text;
    }
    if (type == "BLOB") return ValueType::Blob;
    throw std::runtime_error("Unsupported column type: " + column.type);
}

std::optional<NativeQueryResult> NativeEngine::executeVectorizedSelect(
    const SelectStmt& statement) {
    if (!canVectorize(statement)) return std::nullopt;
    const auto* metadata = catalog_.getTable(statement.fromTable);
    if (!metadata) {
        throw std::runtime_error(
            "Unknown table: " + statement.fromTable);
    }

    struct AggregateState {
        std::string name;
        std::int64_t count = 0;
        double sum = 0.0;
        bool allIntegers = true;
        std::optional<Value> extremum;
        std::unordered_set<Value, ValueHash> distinct;
    };
    struct GroupState {
        Tuple representatives;
        std::vector<AggregateState> aggregates;
    };

    const bool grouped = !statement.groupBy.empty() ||
                         selectHasAggregate(statement);
    std::unordered_map<Tuple, GroupState,
                       TupleKeyHash, TupleKeyEqual> groups;
    NativeQueryResult result;
    result.columns = outputNames(statement, nullptr);
    if (grouped && statement.groupBy.empty()) {
        groups.emplace(Tuple{}, GroupState{});
    }

    auto initializeGroup = [&](GroupState& group) {
        if (!group.representatives.empty() ||
            !group.aggregates.empty()) {
            return;
        }
        group.representatives.resize(
            statement.columns.size(), Value::null());
        group.aggregates.resize(statement.columns.size());
        for (std::size_t index = 0;
             index < statement.columns.size();
             ++index) {
            if (auto* function = dynamic_cast<const FunctionCall*>(
                    statement.columns[index].get())) {
                group.aggregates[index].name = function->name;
            }
        }
    };

    std::set<std::size_t> requiredSet;
    auto requireColumn = [&](const ColumnRef& column) {
        if (!column.table.empty() &&
            column.table != statement.fromTable &&
            column.table != statement.fromAlias) {
            throw std::runtime_error(
                "Unknown table qualifier: " + column.table);
        }
        auto found = std::find_if(
            metadata->columns.begin(), metadata->columns.end(),
            [&](const ColumnMeta& candidate) {
                return candidate.name == column.column;
            });
        if (found == metadata->columns.end()) {
            throw std::runtime_error(
                "Unknown column: " + column.column);
        }
        requiredSet.insert(static_cast<std::size_t>(
            std::distance(metadata->columns.begin(), found)));
    };
    std::function<void(const ASTNode*)> collectColumns =
        [&](const ASTNode* expression) {
            if (!expression || dynamic_cast<const Literal*>(expression)) {
                return;
            }
            if (auto* column =
                    dynamic_cast<const ColumnRef*>(expression)) {
                requireColumn(*column);
                return;
            }
            if (dynamic_cast<const Wildcard*>(expression)) {
                for (std::size_t index = 0;
                     index < metadata->columns.size();
                     ++index) {
                    requiredSet.insert(index);
                }
                return;
            }
            if (auto* binary =
                    dynamic_cast<const BinaryOp*>(expression)) {
                collectColumns(binary->left.get());
                collectColumns(binary->right.get());
                return;
            }
            if (auto* unary =
                    dynamic_cast<const UnaryOp*>(expression)) {
                collectColumns(unary->operand.get());
                return;
            }
            if (auto* function =
                    dynamic_cast<const FunctionCall*>(expression)) {
                if (function->name == "headcount" &&
                    (function->args.empty() ||
                     dynamic_cast<const Wildcard*>(
                         function->args.front().get()))) {
                    return;
                }
                for (const auto& argument : function->args) {
                    collectColumns(argument.get());
                }
            }
        };
    collectColumns(statement.where.get());
    for (const auto& expression : statement.groupBy) {
        collectColumns(expression.get());
    }
    for (const auto& expression : statement.columns) {
        collectColumns(expression.get());
    }
    const std::vector<std::size_t> requiredColumns(
        requiredSet.begin(), requiredSet.end());

    auto file = heap(statement.fromTable);
    ++stats_.tableScans;
    file.scanProjectedBatches(
        requiredColumns, 1024,
        [&](std::vector<StoredRow>&& stored) {
        VectorBatch batch;
        batch.rowCount = stored.size();
        batch.values.reserve(requiredColumns.size());
        batch.columns.reserve(requiredColumns.size());
        for (const auto column : requiredColumns) {
            batch.columns.push_back(
                {statement.fromTable, statement.fromAlias,
                 metadata->columns[column].name});
            auto values = std::make_shared<TypedVector>(
                declaredValueType(metadata->columns[column]));
            values->reserve(batch.rowCount);
            batch.values.push_back(std::move(values));
        }
        for (auto& row : stored) {
            if (row.values.size() != requiredColumns.size()) {
                throw std::runtime_error(
                    "Projected tuple width mismatch in " +
                    statement.fromTable);
            }
            for (std::size_t column = 0;
                 column < row.values.size();
                 ++column) {
                if (row.values[column].isNull()) {
                    ++stats_.vectorNulls;
                }
                batch.values[column]->append(row.values[column]);
            }
        }

        ++stats_.vectorBatches;
        stats_.vectorRows += batch.rowCount;
        stats_.rowsRead += batch.rowCount;
        stats_.decodedColumns +=
            batch.rowCount * requiredColumns.size();
        stats_.skippedColumns +=
            batch.rowCount *
            (metadata->columns.size() - requiredColumns.size());
        auto predicate = statement.where
            ? evalVector(statement.where.get(), batch)
            : std::make_shared<TypedVector>(ValueType::Boolean);
        if (!statement.where) {
            predicate->reserve(batch.rowCount);
            for (std::size_t row = 0;
                 row < batch.rowCount;
                 ++row) {
                predicate->append(Value(true));
            }
        }
        std::vector<VectorPtr> groupVectors;
        for (const auto& expression : statement.groupBy) {
            groupVectors.push_back(
                evalVector(expression.get(), batch));
        }
        std::vector<VectorPtr> projected(
            statement.columns.size());
        for (std::size_t index = 0;
             index < statement.columns.size();
             ++index) {
            const auto* expression = statement.columns[index].get();
            if (auto* function =
                    dynamic_cast<const FunctionCall*>(expression)) {
                if (!function->args.empty() &&
                    !dynamic_cast<const Wildcard*>(
                        function->args.front().get())) {
                    projected[index] = evalVector(
                        function->args.front().get(), batch);
                }
            } else if (!dynamic_cast<const Wildcard*>(expression)) {
                projected[index] = evalVector(expression, batch);
            }
        }

        for (std::size_t row = 0; row < batch.rowCount; ++row) {
            if (!predicate->value(row).asBoolean()) continue;
            if (!grouped) {
                Tuple output;
                for (std::size_t index = 0;
                     index < statement.columns.size();
                     ++index) {
                    if (auto* wildcard =
                            dynamic_cast<const Wildcard*>(
                                statement.columns[index].get())) {
                        for (std::size_t column = 0;
                             column < batch.columns.size();
                             ++column) {
                            if (wildcard->table.empty() ||
                                wildcard->table ==
                                    batch.columns[column].table ||
                                wildcard->table ==
                                    batch.columns[column].alias) {
                                output.push_back(
                                    batch.values[column]->value(row));
                            }
                        }
                    } else {
                        output.push_back(
                            projected[index]->value(row));
                    }
                }
                result.rows.push_back(std::move(output));
                continue;
            }

            Tuple key;
            key.reserve(groupVectors.size());
            for (const auto& values : groupVectors) {
                key.push_back(values->value(row));
            }
            auto& group = groups[key];
            initializeGroup(group);
            for (std::size_t index = 0;
                 index < statement.columns.size();
                 ++index) {
                auto* function =
                    dynamic_cast<const FunctionCall*>(
                        statement.columns[index].get());
                if (!function) {
                    if (group.representatives[index].isNull()) {
                        group.representatives[index] =
                            projected[index]->value(row);
                    }
                    continue;
                }
                auto& state = group.aggregates[index];
                if (function->name == "headcount" &&
                    (function->args.empty() ||
                     dynamic_cast<const Wildcard*>(
                         function->args.front().get()))) {
                    ++state.count;
                    continue;
                }
                const auto& vector = projected[index];
                if (vector->isNull(row)) continue;
                if (!function->distinct &&
                    function->name == "headcount") {
                    ++state.count;
                    continue;
                }
                if (!function->distinct &&
                    (function->name == "stack" ||
                     function->name == "mid") &&
                    (vector->type == ValueType::Integer ||
                     vector->type == ValueType::Real ||
                     vector->type == ValueType::Boolean)) {
                    ++state.count;
                    if (vector->type == ValueType::Integer) {
                        state.sum += static_cast<double>(
                            std::get<std::vector<std::int64_t>>(
                                vector->storage)[row]);
                    } else if (vector->type == ValueType::Real) {
                        state.sum +=
                            std::get<std::vector<double>>(
                                vector->storage)[row];
                    } else {
                        state.sum +=
                            std::get<std::vector<std::uint8_t>>(
                                vector->storage)[row] != 0
                            ? 1.0 : 0.0;
                    }
                    state.allIntegers =
                        state.allIntegers &&
                        vector->type == ValueType::Integer;
                    continue;
                }
                Value value = vector->value(row);
                if (function->distinct &&
                    !state.distinct.insert(value).second) {
                    continue;
                }
                ++state.count;
                if (function->name == "headcount") continue;
                if (function->name == "stack" ||
                    function->name == "mid") {
                    if (!value.isNumeric()) {
                        throw std::runtime_error(
                            "Numeric aggregate on non-number");
                    }
                    state.sum += value.asReal();
                    state.allIntegers =
                        state.allIntegers &&
                        value.type() == ValueType::Integer;
                } else if (!state.extremum) {
                    state.extremum = value;
                } else {
                    const int comparison =
                        value.compare(*state.extremum);
                    if ((function->name == "goat" &&
                         comparison > 0) ||
                        (function->name == "L" &&
                         comparison < 0)) {
                        state.extremum = value;
                    }
                }
            }
        }
    });

    if (grouped) {
        for (auto& item : groups) {
            initializeGroup(item.second);
            Tuple output;
            output.reserve(statement.columns.size());
            for (std::size_t index = 0;
                 index < statement.columns.size();
                 ++index) {
                auto* function =
                    dynamic_cast<const FunctionCall*>(
                        statement.columns[index].get());
                if (!function) {
                    output.push_back(
                        item.second.representatives[index]);
                    continue;
                }
                const auto& state =
                    item.second.aggregates[index];
                if (function->name == "headcount") {
                    output.push_back(Value(state.count));
                } else if (state.count == 0) {
                    output.push_back(Value::null());
                } else if (function->name == "mid") {
                    output.push_back(Value(
                        state.sum /
                        static_cast<double>(state.count)));
                } else if (function->name == "stack") {
                    output.push_back(state.allIntegers
                        ? Value(static_cast<std::int64_t>(state.sum))
                        : Value(state.sum));
                } else {
                    output.push_back(
                        state.extremum.value_or(Value::null()));
                }
            }
            result.rows.push_back(std::move(output));
        }
    }
    ++stats_.vectorizedQueries;
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
    auto file = heap(table);
    file.scanRawRowsFast(
        [&](RowId, const std::uint8_t* data, std::size_t length) {
        ++rows;
        RawField field = decodeRawColumn(data, length, column);
        if (field.isNull) return;
        Value value = field.toValue();
        if (!range.present) {
            range.present = true;
            range.min = value;
            range.max = std::move(value);
        } else {
            if (value.compare(range.min) < 0) range.min = value;
            if (value.compare(range.max) > 0) range.max = std::move(value);
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
