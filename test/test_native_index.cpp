#include "test_framework.h"

#include "native_index.h"

TEST(bplus_tree_inserts_splits_and_finds_keys) {
    BPlusTree tree(3);
    for (int value = 100; value >= 1; --value) {
        tree.insert(Value((std::int64_t)value),
                    RowId{static_cast<std::uint32_t>(value),
                          static_cast<std::uint16_t>(value)});
    }

    ASSERT_EQ(tree.size(), (size_t)100);
    ASSERT_TRUE(tree.height() > 1);
    for (int value = 1; value <= 100; ++value) {
        const auto row = tree.find(Value((std::int64_t)value));
        ASSERT_TRUE(row.has_value());
        ASSERT_EQ(row->page, (std::uint32_t)value);
    }
    ASSERT_FALSE(tree.find(Value((std::int64_t)101)).has_value());
}

TEST(bplus_tree_range_scan_uses_linked_leaves) {
    BPlusTree tree(4);
    for (int value = 1; value <= 20; ++value) {
        tree.insert(Value((std::int64_t)value),
                    RowId{0, static_cast<std::uint16_t>(value)});
    }
    const auto rows = tree.range(
        Value((std::int64_t)5), Value((std::int64_t)10));
    ASSERT_EQ(rows.size(), (size_t)6);
    ASSERT_EQ(rows.front().slot, (std::uint16_t)5);
    ASSERT_EQ(rows.back().slot, (std::uint16_t)10);
}

TEST(bplus_tree_rejects_duplicate_keys) {
    BPlusTree tree;
    tree.insert(Value((std::int64_t)1), RowId{0, 0});
    ASSERT_THROW(
        tree.insert(Value((std::int64_t)1), RowId{0, 1}),
        std::runtime_error);
}

int main() {
    return run_all_tests("Native B+ tree");
}
