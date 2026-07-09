#include "native_engine.h"

#include "native_raw.h"

#include <algorithm>
#include <cstdint>
#include <limits>
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

} // namespace

void NativeEngine::executeSourceOrderJoins(
    const SelectStmt& statement,
    std::vector<EvalRow>& rows) {
    for (const auto& join : statement.joins) {
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
