#include "native_engine.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::toupper(ch));
                   });
    return value;
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
