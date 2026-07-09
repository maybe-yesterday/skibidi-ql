#include "native_engine.h"

#include "native_raw.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

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

} // namespace

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
        if (predicate) {
            const auto normalizedOp = normalizeColumnPredicateOp(
                predicate->op, predicate->columnOnLeft);
            const bool countWildcardOrPredicateColumn =
                !argumentColumn ||
                *argumentColumn == predicate->column;
            if (normalizedOp == "=" &&
                countWildcardOrPredicateColumn &&
                !predicate->literal.isNull()) {
                const auto& range = ensureColumnRange(
                    statement.fromTable,
                    metadata->columns[predicate->column].name);
                if (range.valueCountsExact) {
                    const auto found =
                        range.valueCounts.find(predicate->literal);
                    const auto count = found == range.valueCounts.end()
                        ? std::int64_t{0}
                        : static_cast<std::int64_t>(found->second);
                    NativeQueryResult result;
                    result.columns = outputNames(statement, nullptr);
                    result.rows.push_back(Tuple{Value(count)});
                    ++stats_.directAggregateQueries;
                    ++stats_.valueCountQueries;
                    stats_.valueCountRowsAnswered +=
                        static_cast<std::size_t>(count);
                    return result;
                }
            }
        }
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

    if (!scalarAggregate && groupColumns.size() == 1) {
        const auto groupColumn = groupColumns.front();
        const auto groupType = declaredValueType(
            metadata->columns[groupColumn]);
        bool supportedDenseGroup =
            metadata->columns[groupColumn].not_null &&
            (groupType == ValueType::Integer ||
             groupType == ValueType::Boolean);
        for (const auto& projection : projections) {
            if (!projection.aggregate) {
                supportedDenseGroup =
                    supportedDenseGroup &&
                    projection.representativeColumn &&
                    *projection.representativeColumn == groupColumn;
                continue;
            }
            const auto kind = projection.aggregatePlan.kind;
            supportedDenseGroup =
                supportedDenseGroup &&
                (kind == AggregateKind::Count ||
                 kind == AggregateKind::Sum ||
                 kind == AggregateKind::Average);
        }

        if (supportedDenseGroup) {
            const auto& range = ensureColumnRange(
                statement.fromTable,
                metadata->columns[groupColumn].name);
            if (range.present && range.min.isNumeric() &&
                range.max.isNumeric()) {
                const auto minimum = range.min.asInteger();
                const auto maximum = range.max.asInteger();
                const auto span = maximum - minimum;
                constexpr std::int64_t maxDenseSpan = 4095;
                if (maximum >= minimum && span <= maxDenseSpan) {
                    struct DenseGroupState {
                        bool present = false;
                        Value key = Value::null();
                        std::vector<AggregateState> aggregates;
                    };

                    std::vector<DenseGroupState> denseGroups(
                        static_cast<std::size_t>(span) + 1);
                    for (auto& group : denseGroups) {
                        group.aggregates.resize(projections.size());
                    }

                    auto file = heap(statement.fromTable);
                    ++stats_.tableScans;
                    std::vector<RawField> denseFields;
                    denseFields.reserve(requiredColumns.size());
                    file.scanRawRowsFast(
                        [&](RowId,
                            const std::uint8_t* data,
                            std::size_t length) {
                        decodeRawProjectedInto(
                            data, length, requiredColumns, denseFields);
                        ++stats_.rowsRead;
                        ++stats_.rawRowsScanned;
                        ++stats_.rowCopiesAvoided;
                        stats_.decodedColumns += requiredColumns.size();
                        stats_.skippedColumns +=
                            metadata->columns.size() -
                            requiredColumns.size();
                        if (!predicateMatches(denseFields)) return;

                        const auto& groupField =
                            fieldAt(denseFields, groupColumn);
                        if (groupField.isNull || !groupField.numeric()) {
                            return;
                        }
                        const auto groupValue =
                            groupField.type == ValueType::Boolean
                                ? (groupField.boolean ? 1 : 0)
                                : groupField.integer;
                        if (groupValue < minimum ||
                            groupValue > maximum) {
                            return;
                        }
                        auto& group = denseGroups[
                            static_cast<std::size_t>(
                                groupValue - minimum)];
                        group.present = true;
                        if (group.key.isNull()) {
                            group.key = groupField.toValue();
                        }
                        ++stats_.denseGroupAggregateRows;

                        for (std::size_t index = 0;
                             index < projections.size();
                             ++index) {
                            const auto& projection =
                                projections[index];
                            if (!projection.aggregate) continue;

                            auto& state = group.aggregates[index];
                            const auto& aggregate =
                                projection.aggregatePlan;
                            if (!aggregate.argumentColumn) {
                                ++state.count;
                                continue;
                            }

                            const auto& field = fieldAt(
                                denseFields,
                                *aggregate.argumentColumn);
                            if (field.isNull) continue;
                            if (aggregate.kind ==
                                AggregateKind::Count) {
                                ++state.count;
                                continue;
                            }
                            if (!field.numeric()) {
                                throw std::runtime_error(
                                    "Numeric aggregate on non-number");
                            }
                            ++state.count;
                            state.sum += field.asReal();
                            state.allIntegers =
                                state.allIntegers &&
                                field.type == ValueType::Integer;
                        }
                    });

                    NativeQueryResult result;
                    result.columns = outputNames(statement, nullptr);
                    for (auto& group : denseGroups) {
                        if (!group.present) continue;
                        Tuple output;
                        output.reserve(projections.size());
                        for (std::size_t index = 0;
                             index < projections.size();
                             ++index) {
                            const auto& projection =
                                projections[index];
                            if (!projection.aggregate) {
                                output.push_back(group.key);
                                continue;
                            }
                            const auto& aggregate =
                                projection.aggregatePlan;
                            const auto& state =
                                group.aggregates[index];
                            if (aggregate.kind ==
                                AggregateKind::Count) {
                                output.push_back(Value(state.count));
                            } else if (state.count == 0) {
                                output.push_back(Value::null());
                            } else if (aggregate.kind ==
                                       AggregateKind::Average) {
                                output.push_back(Value(
                                    state.sum /
                                    static_cast<double>(
                                        state.count)));
                            } else {
                                output.push_back(state.allIntegers
                                    ? Value(static_cast<std::int64_t>(
                                          state.sum))
                                    : Value(state.sum));
                            }
                        }
                        result.rows.push_back(std::move(output));
                    }
                    ++stats_.directAggregateQueries;
                    ++stats_.denseGroupAggregateQueries;
                    return result;
                }
            }
        }
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
