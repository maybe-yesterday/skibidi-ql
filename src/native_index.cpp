#include "native_index.h"

#include <algorithm>
#include <stdexcept>

BPlusTree::BPlusTree(std::size_t maxKeys)
    : maxKeys_(std::max<std::size_t>(3, maxKeys)) {
    clear();
}

void BPlusTree::insert(Value key, RowId row) {
    auto split = insert(*root_, std::move(key), row);
    if (split) {
        auto newRoot = std::make_unique<Node>();
        newRoot->leaf = false;
        newRoot->keys.push_back(std::move(split->separator));
        newRoot->children.push_back(std::move(root_));
        newRoot->children.push_back(std::move(split->right));
        root_ = std::move(newRoot);
    }
}

std::optional<RowId> BPlusTree::find(const Value& key) const {
    const Node* leaf = findLeaf(key);
    if (!leaf) return std::nullopt;
    const std::size_t position = lowerBound(leaf->keys, key);
    if (position >= leaf->keys.size() ||
        leaf->keys[position].compare(key) != 0) {
        return std::nullopt;
    }
    return leaf->rows[position];
}

std::vector<RowId> BPlusTree::range(
    const std::optional<Value>& lower,
    const std::optional<Value>& upper) const {
    std::vector<RowId> result;
    const Node* leaf = lower ? findLeaf(*lower) : root_.get();
    while (leaf && !leaf->leaf) leaf = leaf->children.front().get();

    bool first = true;
    while (leaf) {
        std::size_t start = 0;
        if (first && lower) start = lowerBound(leaf->keys, *lower);
        first = false;
        for (std::size_t index = start; index < leaf->keys.size(); ++index) {
            if (upper && leaf->keys[index].compare(*upper) > 0) {
                return result;
            }
            result.push_back(leaf->rows[index]);
        }
        leaf = leaf->next;
    }
    return result;
}

void BPlusTree::clear() {
    root_ = std::make_unique<Node>();
    root_->leaf = true;
    size_ = 0;
}

std::size_t BPlusTree::height() const {
    std::size_t result = 0;
    const Node* node = root_.get();
    while (node) {
        ++result;
        node = node->leaf ? nullptr : node->children.front().get();
    }
    return result;
}

std::optional<BPlusTree::Split> BPlusTree::insert(
    Node& node,
    Value key,
    RowId row) {
    if (node.leaf) {
        const std::size_t position = lowerBound(node.keys, key);
        if (position < node.keys.size() &&
            node.keys[position].compare(key) == 0) {
            throw std::runtime_error("Duplicate B+ tree key");
        }
        node.keys.insert(node.keys.begin() + position, std::move(key));
        node.rows.insert(node.rows.begin() + position, row);
        ++size_;
        if (node.keys.size() <= maxKeys_) return std::nullopt;

        const std::size_t middle = node.keys.size() / 2;
        auto right = std::make_unique<Node>();
        right->leaf = true;
        right->keys.assign(
            std::make_move_iterator(node.keys.begin() + middle),
            std::make_move_iterator(node.keys.end()));
        right->rows.assign(node.rows.begin() + middle, node.rows.end());
        node.keys.resize(middle);
        node.rows.resize(middle);
        right->next = node.next;
        node.next = right.get();
        Split split{right->keys.front(), std::move(right)};
        return split;
    }

    const std::size_t childIndex = upperBound(node.keys, key);
    auto split = insert(*node.children[childIndex], std::move(key), row);
    if (!split) return std::nullopt;

    node.keys.insert(node.keys.begin() + childIndex,
                     std::move(split->separator));
    node.children.insert(node.children.begin() + childIndex + 1,
                         std::move(split->right));
    if (node.keys.size() <= maxKeys_) return std::nullopt;

    const std::size_t middle = node.keys.size() / 2;
    Value separator = node.keys[middle];
    auto right = std::make_unique<Node>();
    right->leaf = false;
    right->keys.assign(
        std::make_move_iterator(node.keys.begin() + middle + 1),
        std::make_move_iterator(node.keys.end()));
    right->children.assign(
        std::make_move_iterator(node.children.begin() + middle + 1),
        std::make_move_iterator(node.children.end()));
    node.keys.resize(middle);
    node.children.resize(middle + 1);
    return Split{std::move(separator), std::move(right)};
}

const BPlusTree::Node* BPlusTree::findLeaf(const Value& key) const {
    const Node* node = root_.get();
    while (node && !node->leaf) {
        node = node->children[upperBound(node->keys, key)].get();
    }
    return node;
}

std::size_t BPlusTree::lowerBound(const std::vector<Value>& keys,
                                  const Value& key) {
    return static_cast<std::size_t>(std::lower_bound(
        keys.begin(), keys.end(), key,
        [](const Value& left, const Value& right) {
            return left.compare(right) < 0;
        }) - keys.begin());
}

std::size_t BPlusTree::upperBound(const std::vector<Value>& keys,
                                  const Value& key) {
    return static_cast<std::size_t>(std::upper_bound(
        keys.begin(), keys.end(), key,
        [](const Value& left, const Value& right) {
            return left.compare(right) < 0;
        }) - keys.begin());
}
