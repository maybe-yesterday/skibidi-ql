#include "native_engine.h"

#include "native_raw.h"

#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

std::optional<NativeQueryResult> NativeEngine::executeRawPointSelect(
    const SelectStmt& statement) {
    const bool positiveLimit = !statement.limit ||
        integerLiteral(statement.limit.get(), -1) > 0;
    if (!statement.joins.empty() || statement.distinct ||
        statement.having || !statement.orderBy.empty() ||
        !positiveLimit || statement.offset ||
        !statement.groupBy.empty() ||
        selectHasAggregate(statement)) {
        return std::nullopt;
    }

    const auto* predicate =
        dynamic_cast<const BinaryOp*>(statement.where.get());
    if (!predicate || predicate->op != "=") return std::nullopt;

    const auto key = primaryKeyPredicate(
        statement.fromTable, statement.fromAlias, statement.where.get());
    if (!key) return std::nullopt;

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

    std::vector<std::size_t> projectionColumns;
    std::set<std::size_t> requiredSet;
    for (const auto& expression : statement.columns) {
        if (const auto* wildcard =
                dynamic_cast<const Wildcard*>(expression.get())) {
            if (!wildcard->table.empty() &&
                wildcard->table != statement.fromTable &&
                wildcard->table != statement.fromAlias) {
                return std::nullopt;
            }
            for (std::size_t index = 0;
                 index < metadata->columns.size();
                 ++index) {
                projectionColumns.push_back(index);
                requiredSet.insert(index);
            }
            continue;
        }

        const auto* column =
            dynamic_cast<const ColumnRef*>(expression.get());
        if (!column) return std::nullopt;
        const auto index = resolveColumnIndex(*column);
        projectionColumns.push_back(index);
        requiredSet.insert(index);
    }

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

    NativeQueryResult result;
    result.columns = outputNames(statement, nullptr);
    ++stats_.rawPointQueries;
    ++stats_.indexLookups;
    const auto rowId = primaryIndex(statement.fromTable).find(*key);
    if (!rowId) return result;

    std::vector<RawField> fields;
    auto file = heap(statement.fromTable);
    const bool read = file.readRawRowFast(
        *rowId,
        [&](const std::uint8_t* data, std::size_t length) {
        decodeRawProjectedInto(data, length, requiredColumns, fields);
    });
    if (!read) {
        primaryIndexes_.erase(statement.fromTable);
        return std::nullopt;
    }

    Tuple row;
    row.reserve(projectionColumns.size());
    for (const auto column : projectionColumns) {
        const auto position = fieldPositions[column];
        if (position == missing || position >= fields.size()) {
            throw std::runtime_error(
                "Raw point column was not decoded");
        }
        row.push_back(fields[position].toValue());
    }
    result.rows.push_back(std::move(row));
    ++stats_.rawPointHits;
    ++stats_.rowsRead;
    ++stats_.rowCopiesAvoided;
    stats_.decodedColumns += requiredColumns.size();
    stats_.skippedColumns +=
        metadata->columns.size() - requiredColumns.size();
    return result;
}
