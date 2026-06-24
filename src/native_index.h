#pragma once

#include "native_storage.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

class BPlusTree {
public:
    explicit BPlusTree(std::size_t maxKeys = 32);

    void insert(Value key, RowId row);
    std::optional<RowId> find(const Value& key) const;
    std::vector<RowId> range(const std::optional<Value>& lower,
                             const std::optional<Value>& upper) const;
    void clear();
    std::size_t size() const { return size_; }
    std::size_t height() const;

private:
    struct Node {
        bool leaf = false;
        std::vector<Value> keys;
        std::vector<std::unique_ptr<Node>> children;
        std::vector<RowId> rows;
        Node* next = nullptr;
    };

    struct Split {
        Value separator;
        std::unique_ptr<Node> right;
    };

    std::size_t maxKeys_;
    std::size_t size_ = 0;
    std::unique_ptr<Node> root_;

    std::optional<Split> insert(Node& node, Value key, RowId row);
    const Node* findLeaf(const Value& key) const;
    static std::size_t lowerBound(const std::vector<Value>& keys,
                                  const Value& key);
    static std::size_t upperBound(const std::vector<Value>& keys,
                                  const Value& key);
};
