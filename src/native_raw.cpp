#include "native_raw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {

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

} // namespace

bool RawField::numeric() const {
    return !isNull &&
           (type == ValueType::Integer ||
            type == ValueType::Real ||
            type == ValueType::Boolean);
}

double RawField::asReal() const {
    if (type == ValueType::Real) return real;
    if (type == ValueType::Integer) {
        return static_cast<double>(integer);
    }
    if (type == ValueType::Boolean) return boolean ? 1.0 : 0.0;
    throw std::runtime_error("Raw field is not numeric");
}

Value RawField::toValue() const {
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

std::string normalizeColumnPredicateOp(std::string op,
                                       bool columnOnLeft) {
    if (columnOnLeft) return op;
    if (op == "<") return ">";
    if (op == ">") return "<";
    if (op == "<=") return ">=";
    if (op == ">=") return "<=";
    return op;
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
