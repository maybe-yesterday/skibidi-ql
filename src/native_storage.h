#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

enum class ValueType : std::uint8_t {
    Null = 0,
    Integer = 1,
    Real = 2,
    Text = 3,
    Boolean = 4,
    Blob = 5
};

class Value {
public:
    using Blob = std::vector<std::uint8_t>;

    Value();
    explicit Value(std::int64_t value);
    explicit Value(double value);
    explicit Value(std::string value);
    explicit Value(bool value);
    explicit Value(Blob value);

    static Value null();

    ValueType type() const;
    bool isNull() const;
    bool isNumeric() const;
    std::int64_t asInteger() const;
    double asReal() const;
    bool asBoolean() const;
    const std::string& asText() const;
    const Blob& asBlob() const;
    std::string toString() const;

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }
    int compare(const Value& other) const;
    std::size_t hash() const;

private:
    std::variant<std::monostate, std::int64_t, double,
                 std::string, bool, Blob> data_;
};

struct ValueHash {
    std::size_t operator()(const Value& value) const {
        return value.hash();
    }
};

using Tuple = std::vector<Value>;

struct RowId {
    std::uint32_t page = 0;
    std::uint16_t slot = 0;

    bool operator==(const RowId& other) const {
        return page == other.page && slot == other.slot;
    }
};

struct StoredRow {
    RowId id;
    Tuple values;
};

class SlottedPage {
public:
    static constexpr std::size_t PAGE_SIZE = 4096;
    static constexpr std::uint32_t MAGIC = 0x534B5047;

    struct RowView {
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;
    };

    explicit SlottedPage(std::array<std::uint8_t, PAGE_SIZE>& bytes);

    void initialize();
    bool valid() const;
    std::size_t freeSpace() const;
    std::size_t slotCount() const;
    std::optional<std::uint16_t> insert(const std::vector<std::uint8_t>& row);
    bool erase(std::uint16_t slot);
    std::optional<std::vector<std::uint8_t>> read(std::uint16_t slot) const;
    std::optional<RowView> readView(std::uint16_t slot) const;
    bool update(std::uint16_t slot, const std::vector<std::uint8_t>& row);

private:
    std::array<std::uint8_t, PAGE_SIZE>& bytes_;

    std::uint16_t read16(std::size_t offset) const;
    std::uint32_t read32(std::size_t offset) const;
    void write16(std::size_t offset, std::uint16_t value);
    void write32(std::size_t offset, std::uint32_t value);
    std::size_t slotOffset(std::uint16_t slot) const;
    void compact();
};

class BufferPool {
private:
    struct Frame;

public:
    class PageGuard {
    public:
        PageGuard() = default;
        ~PageGuard();
        PageGuard(const PageGuard&) = delete;
        PageGuard& operator=(const PageGuard&) = delete;
        PageGuard(PageGuard&& other) noexcept;
        PageGuard& operator=(PageGuard&& other) noexcept;

        std::array<std::uint8_t, SlottedPage::PAGE_SIZE>& bytes();
        const std::array<std::uint8_t, SlottedPage::PAGE_SIZE>& bytes() const;
        void markDirty();
        explicit operator bool() const { return frame_ != nullptr; }

    private:
        friend class BufferPool;
        PageGuard(BufferPool* pool, Frame* frame);
        void release();

        BufferPool* pool_ = nullptr;
        Frame* frame_ = nullptr;
    };

    explicit BufferPool(std::size_t capacityPages = 1024);
    ~BufferPool();

    PageGuard fetch(const std::filesystem::path& file,
                    std::uint32_t pageNumber,
                    bool create);
    void flushAll();
    void discardAll();
    void flushFile(const std::filesystem::path& file);
    void invalidateFile(const std::filesystem::path& file);
    std::size_t residentPages() const;
    std::size_t capacityPages() const { return capacityPages_; }
    std::size_t pageReads() const { return pageReads_; }
    std::size_t evictions() const { return evictions_; }
    void resetStats() {
        pageReads_ = 0;
        evictions_ = 0;
    }

private:
    struct PageKey {
        std::string file;
        std::uint32_t page = 0;

        bool operator==(const PageKey& other) const {
            return file == other.file && page == other.page;
        }
    };

    struct PageKeyHash {
        std::size_t operator()(const PageKey& key) const;
    };

    struct Frame {
        PageKey key;
        std::array<std::uint8_t, SlottedPage::PAGE_SIZE> bytes{};
        bool dirty = false;
        std::size_t pins = 0;
        std::list<PageKey>::iterator lru;
    };

    std::size_t capacityPages_;
    std::unordered_map<PageKey, std::unique_ptr<Frame>,
                       PageKeyHash> frames_;
    std::list<PageKey> lru_;
    std::size_t pageReads_ = 0;
    std::size_t evictions_ = 0;

    static std::string canonicalPath(const std::filesystem::path& path);
    void unpin(Frame* frame);
    void touch(Frame* frame);
    void evictOne();
    void flush(Frame& frame);
    static void readPage(const PageKey& key,
                         std::array<std::uint8_t,
                                    SlottedPage::PAGE_SIZE>& bytes,
                         bool create);
    static void writePage(const PageKey& key,
                          const std::array<std::uint8_t,
                                           SlottedPage::PAGE_SIZE>& bytes);
};

class MappedHeapFile {
public:
    MappedHeapFile() = default;
    ~MappedHeapFile();
    MappedHeapFile(const MappedHeapFile&) = delete;
    MappedHeapFile& operator=(const MappedHeapFile&) = delete;
    MappedHeapFile(MappedHeapFile&& other) noexcept;
    MappedHeapFile& operator=(MappedHeapFile&& other) noexcept;

    static MappedHeapFile open(const std::filesystem::path& path);
    bool isMapped() const { return data_ != nullptr && size_ != 0; }
    std::size_t size() const { return size_; }
    std::size_t pageCount() const {
        return size_ / SlottedPage::PAGE_SIZE;
    }

    template <typename Visitor>
    void scanRawRowsFast(Visitor&& visitor) const {
        if (!isMapped()) return;
        const std::size_t pages = pageCount();
        for (std::uint32_t page = 0; page < pages; ++page) {
            const auto* bytes = data_ +
                static_cast<std::size_t>(page) * SlottedPage::PAGE_SIZE;
            if (!validPage(bytes)) continue;
            const auto slots = slotCount(bytes);
            for (std::uint16_t slot = 0; slot < slots; ++slot) {
                auto view = readView(bytes, slot);
                if (!view) continue;
                visitor(RowId{page, slot}, view->data, view->size);
            }
        }
    }

    template <typename Visitor>
    bool readRawRowFast(RowId row, Visitor&& visitor) const {
        if (!isMapped() || row.page >= pageCount()) return false;
        const auto* bytes = data_ +
            static_cast<std::size_t>(row.page) * SlottedPage::PAGE_SIZE;
        if (!validPage(bytes)) return false;
        auto view = readView(bytes, row.slot);
        if (!view) return false;
        visitor(view->data, view->size);
        return true;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
#ifdef _WIN32
    void* fileHandle_ = nullptr;
    void* mappingHandle_ = nullptr;
#else
    int fileDescriptor_ = -1;
#endif

    void close();
    static std::uint16_t read16(const std::uint8_t* bytes,
                                std::size_t offset);
    static std::uint32_t read32(const std::uint8_t* bytes,
                                std::size_t offset);
    static bool validPage(const std::uint8_t* bytes);
    static std::uint16_t slotCount(const std::uint8_t* bytes);
    static std::optional<SlottedPage::RowView> readView(
        const std::uint8_t* bytes,
        std::uint16_t slot);
};

class HeapFile {
public:
    HeapFile(std::filesystem::path path, BufferPool& bufferPool);

    void create();
    void drop();
    bool exists() const;
    std::size_t pageCount() const;

    RowId insert(const Tuple& tuple);
    std::vector<StoredRow> scan();
    void scanBatches(
        std::size_t batchSize,
        const std::function<void(std::vector<StoredRow>&&)>& visitor);
    void scanProjectedBatches(
        const std::vector<std::size_t>& columns,
        std::size_t batchSize,
        const std::function<void(std::vector<StoredRow>&&)>& visitor);
    void scanRawRows(
        const std::function<void(RowId,
                                 const std::uint8_t*,
                                 std::size_t)>& visitor);
    template <typename Visitor>
    void scanRawRowsFast(Visitor&& visitor) {
        const std::size_t pages = pageCount();
        for (std::uint32_t page = 0; page < pages; ++page) {
            auto guard = bufferPool_.fetch(path_, page, false);
            SlottedPage slotted(guard.bytes());
            if (!slotted.valid()) continue;
            for (std::uint16_t slot = 0;
                 slot < slotted.slotCount();
                 ++slot) {
                auto view = slotted.readView(slot);
                if (!view) continue;
                visitor(RowId{page, slot}, view->data, view->size);
            }
        }
    }
    template <typename Visitor>
    bool readRawRowFast(RowId row, Visitor&& visitor) {
        auto guard = bufferPool_.fetch(path_, row.page, false);
        SlottedPage slotted(guard.bytes());
        if (!slotted.valid()) return false;
        auto view = slotted.readView(row.slot);
        if (!view) return false;
        visitor(view->data, view->size);
        return true;
    }
    std::optional<StoredRow> read(RowId row);
    bool erase(RowId row);
    RowId update(RowId row, const Tuple& tuple);
    void flush();
    const std::filesystem::path& path() const { return path_; }
    MappedHeapFile mappedView() const;

    static std::vector<std::uint8_t> encodeTuple(const Tuple& tuple);
    static Tuple decodeTuple(const std::vector<std::uint8_t>& bytes);
    static Tuple decodeTupleProjected(
        const std::vector<std::uint8_t>& bytes,
        const std::vector<std::size_t>& columns);

private:
    std::filesystem::path path_;
    BufferPool& bufferPool_;
    std::optional<std::uint32_t> insertPageHint_;
    mutable std::optional<std::size_t> knownPageCount_;
};
