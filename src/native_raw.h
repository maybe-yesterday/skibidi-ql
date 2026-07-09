#pragma once

#include "ast.h"
#include "native_storage.h"

#include <cstdint>
#include <string>
#include <vector>

struct RawField {
    ValueType type = ValueType::Null;
    bool isNull = true;
    std::int64_t integer = 0;
    double real = 0.0;
    bool boolean = false;
    std::string text;
    Value::Blob blob;

    bool numeric() const;
    double asReal() const;
    Value toValue() const;
};

std::int64_t integerLiteral(const ASTNode* expression,
                            std::int64_t fallback);
Value literalValue(const Literal& literal);

void decodeRawProjectedInto(const std::uint8_t* data,
                            std::size_t length,
                            const std::vector<std::size_t>& columns,
                            std::vector<RawField>& fields);
RawField decodeRawColumn(const std::uint8_t* data,
                         std::size_t length,
                         std::size_t column);

int compareRawToValue(const RawField& left, const Value& right);
bool comparisonMatches(const std::string& op, int order);
std::string normalizeColumnPredicateOp(std::string op,
                                       bool columnOnLeft);

bool isLoneWolfName(const std::string& name);
bool isSimpleAggregateName(const std::string& name);
std::int64_t countLoneWolves(const std::vector<double>& samples);
