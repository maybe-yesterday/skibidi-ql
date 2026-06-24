#include "test_framework.h"

#include "native_storage.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <string>

struct TempDirectory {
    std::filesystem::path path;

    TempDirectory() {
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("skibidi-storage-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

TEST(value_numeric_comparison_and_hash_are_consistent) {
    Value integer((std::int64_t)1);
    Value real(1.0);
    ASSERT_TRUE(integer == real);
    ASSERT_EQ(integer.hash(), real.hash());
    ASSERT_EQ(integer.compare(real), 0);
}

TEST(tuple_encoding_roundtrip_preserves_types) {
    Tuple tuple{
        Value((std::int64_t)42),
        Value(3.5),
        Value(std::string("hello")),
        Value(true),
        Value::null(),
        Value(Value::Blob{1, 2, 3})
    };

    const auto encoded = HeapFile::encodeTuple(tuple);
    const auto decoded = HeapFile::decodeTuple(encoded);
    ASSERT_EQ(decoded.size(), tuple.size());
    for (std::size_t i = 0; i < tuple.size(); ++i) {
        ASSERT_TRUE(decoded[i] == tuple[i]);
    }
}

TEST(projected_tuple_decode_skips_unrequested_fields) {
    Tuple tuple{
        Value((std::int64_t)7),
        Value(std::string(2048, 'x')),
        Value::null(),
        Value(9.5),
        Value(Value::Blob{1, 2, 3, 4})
    };

    const auto encoded = HeapFile::encodeTuple(tuple);
    const auto projected =
        HeapFile::decodeTupleProjected(encoded, {0, 2, 3});
    ASSERT_EQ(projected.size(), (size_t)3);
    ASSERT_TRUE(projected[0] == Value((std::int64_t)7));
    ASSERT_TRUE(projected[1].isNull());
    ASSERT_TRUE(projected[2] == Value(9.5));

    const auto noColumns =
        HeapFile::decodeTupleProjected(encoded, {});
    ASSERT_TRUE(noColumns.empty());
}

TEST(projected_heap_scan_returns_only_requested_columns) {
    TempDirectory temp;
    BufferPool pool(2);
    HeapFile file(temp.path / "projected.heap", pool);
    file.insert(Tuple{
        Value((std::int64_t)1),
        Value(std::string("unused")),
        Value(4.5)});
    file.insert(Tuple{
        Value((std::int64_t)2),
        Value(std::string("also-unused")),
        Value::null()});

    std::vector<StoredRow> rows;
    file.scanProjectedBatches(
        {0, 2}, 8,
        [&](std::vector<StoredRow>&& batch) {
            rows = std::move(batch);
        });
    ASSERT_EQ(rows.size(), (size_t)2);
    ASSERT_EQ(rows[0].values.size(), (size_t)2);
    ASSERT_TRUE(rows[0].values[0] == Value((std::int64_t)1));
    ASSERT_TRUE(rows[0].values[1] == Value(4.5));
    ASSERT_TRUE(rows[1].values[1].isNull());
}

TEST(slotted_page_insert_read_update_delete) {
    std::array<std::uint8_t, SlottedPage::PAGE_SIZE> bytes{};
    SlottedPage page(bytes);
    page.initialize();

    const auto first = HeapFile::encodeTuple(
        Tuple{Value((std::int64_t)1), Value(std::string("Ada"))});
    const auto slot = page.insert(first);
    ASSERT_TRUE(slot.has_value());
    ASSERT_EQ(page.slotCount(), (size_t)1);

    auto read = page.read(*slot);
    ASSERT_TRUE(read.has_value());
    ASSERT_TRUE(HeapFile::decodeTuple(*read)[1] ==
                Value(std::string("Ada")));

    const auto shorter = HeapFile::encodeTuple(
        Tuple{Value((std::int64_t)1), Value(std::string("A"))});
    ASSERT_TRUE(page.update(*slot, shorter));
    ASSERT_TRUE(page.erase(*slot));
    ASSERT_FALSE(page.read(*slot).has_value());
}

TEST(heap_file_persists_across_buffer_pools) {
    TempDirectory temp;
    const auto path = temp.path / "users.heap";
    {
        BufferPool pool(2);
        HeapFile file(path, pool);
        file.insert(Tuple{Value((std::int64_t)1),
                          Value(std::string("Ada"))});
        file.insert(Tuple{Value((std::int64_t)2),
                          Value(std::string("Grace"))});
        file.flush();
    }
    {
        BufferPool pool(1);
        HeapFile file(path, pool);
        const auto rows = file.scan();
        ASSERT_EQ(rows.size(), (size_t)2);
        ASSERT_TRUE(rows[0].values[0] == Value((std::int64_t)1));
        ASSERT_TRUE(rows[1].values[1] == Value(std::string("Grace")));
    }
}

TEST(heap_file_update_and_delete) {
    TempDirectory temp;
    BufferPool pool(2);
    HeapFile file(temp.path / "items.heap", pool);
    const RowId first =
        file.insert(Tuple{Value((std::int64_t)1),
                          Value(std::string("short"))});
    const RowId second =
        file.insert(Tuple{Value((std::int64_t)2),
                          Value(std::string("keep"))});

    file.update(first, Tuple{Value((std::int64_t)1),
                             Value(std::string(100, 'x'))});
    ASSERT_TRUE(file.erase(second));
    file.flush();

    const auto rows = file.scan();
    ASSERT_EQ(rows.size(), (size_t)1);
    ASSERT_EQ(rows[0].values[1].asText().size(), (size_t)100);
}

int main() {
    return run_all_tests("Native storage");
}
