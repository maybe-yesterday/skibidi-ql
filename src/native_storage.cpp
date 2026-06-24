#include "native_storage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

template <typename T>
void appendPod(std::vector<std::uint8_t>& out, const T& value) {
    const auto* bytes =
        reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
T readPod(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("Corrupt tuple payload");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

std::size_t combineHash(std::size_t seed, std::size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL +
                   (seed << 6) + (seed >> 2));
}

} // namespace

Value::Value() : data_(std::monostate{}) {}
Value::Value(std::int64_t value) : data_(value) {}
Value::Value(double value) : data_(value) {}
Value::Value(std::string value) : data_(std::move(value)) {}
Value::Value(bool value) : data_(value) {}
Value::Value(Blob value) : data_(std::move(value)) {}

Value Value::null() {
    return Value();
}

ValueType Value::type() const {
    switch (data_.index()) {
        case 0: return ValueType::Null;
        case 1: return ValueType::Integer;
        case 2: return ValueType::Real;
        case 3: return ValueType::Text;
        case 4: return ValueType::Boolean;
        case 5: return ValueType::Blob;
        default: throw std::runtime_error("Invalid value type");
    }
}

bool Value::isNull() const {
    return type() == ValueType::Null;
}

bool Value::isNumeric() const {
    return type() == ValueType::Integer ||
           type() == ValueType::Real ||
           type() == ValueType::Boolean;
}

std::int64_t Value::asInteger() const {
    if (auto* value = std::get_if<std::int64_t>(&data_)) return *value;
    if (auto* value = std::get_if<double>(&data_)) {
        return static_cast<std::int64_t>(*value);
    }
    if (auto* value = std::get_if<bool>(&data_)) return *value ? 1 : 0;
    throw std::runtime_error("Value is not numeric");
}

double Value::asReal() const {
    if (auto* value = std::get_if<double>(&data_)) return *value;
    if (auto* value = std::get_if<std::int64_t>(&data_)) {
        return static_cast<double>(*value);
    }
    if (auto* value = std::get_if<bool>(&data_)) return *value ? 1.0 : 0.0;
    throw std::runtime_error("Value is not numeric");
}

bool Value::asBoolean() const {
    if (isNull()) return false;
    if (auto* value = std::get_if<bool>(&data_)) return *value;
    if (isNumeric()) return asReal() != 0.0;
    if (auto* value = std::get_if<std::string>(&data_)) {
        return !value->empty();
    }
    if (auto* value = std::get_if<Blob>(&data_)) return !value->empty();
    return false;
}

const std::string& Value::asText() const {
    auto* value = std::get_if<std::string>(&data_);
    if (!value) throw std::runtime_error("Value is not text");
    return *value;
}

const Value::Blob& Value::asBlob() const {
    auto* value = std::get_if<Blob>(&data_);
    if (!value) throw std::runtime_error("Value is not a blob");
    return *value;
}

std::string Value::toString() const {
    switch (type()) {
        case ValueType::Null:
            return "NULL";
        case ValueType::Integer:
            return std::to_string(std::get<std::int64_t>(data_));
        case ValueType::Real: {
            std::ostringstream out;
            out << std::setprecision(15) << std::get<double>(data_);
            return out.str();
        }
        case ValueType::Text:
            return std::get<std::string>(data_);
        case ValueType::Boolean:
            return std::get<bool>(data_) ? "1" : "0";
        case ValueType::Blob: {
            std::ostringstream out;
            out << "0x" << std::hex << std::setfill('0');
            for (std::uint8_t byte : std::get<Blob>(data_)) {
                out << std::setw(2) << static_cast<int>(byte);
            }
            return out.str();
        }
    }
    return "";
}

bool Value::operator==(const Value& other) const {
    if (isNull() || other.isNull()) {
        return isNull() && other.isNull();
    }
    if (isNumeric() && other.isNumeric()) {
        return asReal() == other.asReal();
    }
    return data_ == other.data_;
}

int Value::compare(const Value& other) const {
    if (isNull() || other.isNull()) {
        if (isNull() && other.isNull()) return 0;
        return isNull() ? -1 : 1;
    }
    if (isNumeric() && other.isNumeric()) {
        const double left = asReal();
        const double right = other.asReal();
        return left < right ? -1 : (left > right ? 1 : 0);
    }
    if (type() != other.type()) {
        return static_cast<int>(type()) < static_cast<int>(other.type())
            ? -1 : 1;
    }
    if (type() == ValueType::Text) {
        const int result = asText().compare(other.asText());
        return result < 0 ? -1 : (result > 0 ? 1 : 0);
    }
    if (type() == ValueType::Blob) {
        const auto& left = asBlob();
        const auto& right = other.asBlob();
        if (left == right) return 0;
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end()) ? -1 : 1;
    }
    if (type() == ValueType::Boolean) {
        const bool left = asBoolean();
        const bool right = other.asBoolean();
        return left == right ? 0 : (left ? 1 : -1);
    }
    return 0;
}

std::size_t Value::hash() const {
    std::size_t seed = static_cast<std::size_t>(type());
    if (isNumeric()) {
        return combineHash(static_cast<std::size_t>(ValueType::Real),
                           std::hash<double>{}(asReal()));
    }
    switch (type()) {
        case ValueType::Null:
            return seed;
        case ValueType::Real:
        case ValueType::Integer:
        case ValueType::Boolean:
            return seed;
        case ValueType::Text:
            return combineHash(seed,
                std::hash<std::string>{}(asText()));
        case ValueType::Blob: {
            for (std::uint8_t byte : asBlob()) {
                seed = combineHash(seed, byte);
            }
            return seed;
        }
    }
    return seed;
}

SlottedPage::SlottedPage(
    std::array<std::uint8_t, PAGE_SIZE>& bytes)
    : bytes_(bytes) {}

void SlottedPage::initialize() {
    bytes_.fill(0);
    write32(0, MAGIC);
    write16(4, 0);
    write16(6, 16);
    write16(8, static_cast<std::uint16_t>(PAGE_SIZE));
}

bool SlottedPage::valid() const {
    return read32(0) == MAGIC;
}

std::size_t SlottedPage::freeSpace() const {
    if (!valid()) return 0;
    const std::uint16_t start = read16(6);
    const std::uint16_t end = read16(8);
    return end >= start ? end - start : 0;
}

std::size_t SlottedPage::slotCount() const {
    return valid() ? read16(4) : 0;
}

std::optional<std::uint16_t> SlottedPage::insert(
    const std::vector<std::uint8_t>& row) {
    if (!valid()) initialize();
    if (row.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("Tuple is too large");
    }

    std::optional<std::uint16_t> reusable;
    for (std::uint16_t slot = 0; slot < slotCount(); ++slot) {
        if (read16(slotOffset(slot) + 4) == 0) {
            reusable = slot;
            break;
        }
    }
    const std::size_t slotBytes = reusable ? 0 : 6;
    if (freeSpace() < row.size() + slotBytes) {
        compact();
    }
    if (freeSpace() < row.size() + slotBytes) return std::nullopt;

    const std::uint16_t newEnd =
        static_cast<std::uint16_t>(read16(8) - row.size());
    std::memcpy(bytes_.data() + newEnd, row.data(), row.size());
    write16(8, newEnd);

    std::uint16_t slot = 0;
    if (reusable) {
        slot = *reusable;
    } else {
        slot = read16(4);
        write16(4, static_cast<std::uint16_t>(slot + 1));
        write16(6, static_cast<std::uint16_t>(16 + (slot + 1) * 6));
    }
    const std::size_t position = slotOffset(slot);
    write16(position, newEnd);
    write16(position + 2, static_cast<std::uint16_t>(row.size()));
    write16(position + 4, 1);
    return slot;
}

bool SlottedPage::erase(std::uint16_t slot) {
    if (!valid() || slot >= slotCount()) return false;
    const std::size_t position = slotOffset(slot);
    if (read16(position + 4) == 0) return false;
    write16(position + 4, 0);
    write16(position + 2, 0);
    return true;
}

std::optional<std::vector<std::uint8_t>> SlottedPage::read(
    std::uint16_t slot) const {
    if (!valid() || slot >= slotCount()) return std::nullopt;
    const std::size_t position = slotOffset(slot);
    if (read16(position + 4) == 0) return std::nullopt;
    const std::uint16_t offset = read16(position);
    const std::uint16_t length = read16(position + 2);
    if (offset + length > PAGE_SIZE) {
        throw std::runtime_error("Corrupt slotted page");
    }
    return std::vector<std::uint8_t>(
        bytes_.begin() + offset,
        bytes_.begin() + offset + length);
}

bool SlottedPage::update(
    std::uint16_t slot,
    const std::vector<std::uint8_t>& row) {
    if (!valid() || slot >= slotCount()) return false;
    const std::size_t position = slotOffset(slot);
    if (read16(position + 4) == 0) return false;
    const std::uint16_t offset = read16(position);
    const std::uint16_t length = read16(position + 2);
    if (row.size() > length) return false;
    std::memcpy(bytes_.data() + offset, row.data(), row.size());
    write16(position + 2, static_cast<std::uint16_t>(row.size()));
    return true;
}

std::uint16_t SlottedPage::read16(std::size_t offset) const {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes_.data() + offset, sizeof(value));
    return value;
}

std::uint32_t SlottedPage::read32(std::size_t offset) const {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes_.data() + offset, sizeof(value));
    return value;
}

void SlottedPage::write16(std::size_t offset, std::uint16_t value) {
    std::memcpy(bytes_.data() + offset, &value, sizeof(value));
}

void SlottedPage::write32(std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes_.data() + offset, &value, sizeof(value));
}

std::size_t SlottedPage::slotOffset(std::uint16_t slot) const {
    return 16 + static_cast<std::size_t>(slot) * 6;
}

void SlottedPage::compact() {
    std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>> rows;
    for (std::uint16_t slot = 0; slot < slotCount(); ++slot) {
        auto row = read(slot);
        if (row) rows.emplace_back(slot, std::move(*row));
    }
    std::uint16_t end = static_cast<std::uint16_t>(PAGE_SIZE);
    for (auto& item : rows) {
        end = static_cast<std::uint16_t>(end - item.second.size());
        std::memcpy(bytes_.data() + end,
                    item.second.data(),
                    item.second.size());
        const std::size_t position = slotOffset(item.first);
        write16(position, end);
        write16(position + 2,
                static_cast<std::uint16_t>(item.second.size()));
        write16(position + 4, 1);
    }
    write16(8, end);
}

BufferPool::PageGuard::PageGuard(BufferPool* pool, Frame* frame)
    : pool_(pool), frame_(frame) {}

BufferPool::PageGuard::~PageGuard() {
    release();
}

BufferPool::PageGuard::PageGuard(PageGuard&& other) noexcept
    : pool_(other.pool_), frame_(other.frame_) {
    other.pool_ = nullptr;
    other.frame_ = nullptr;
}

BufferPool::PageGuard& BufferPool::PageGuard::operator=(
    PageGuard&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = other.pool_;
        frame_ = other.frame_;
        other.pool_ = nullptr;
        other.frame_ = nullptr;
    }
    return *this;
}

std::array<std::uint8_t, SlottedPage::PAGE_SIZE>&
BufferPool::PageGuard::bytes() {
    if (!frame_) throw std::runtime_error("Invalid page guard");
    return frame_->bytes;
}

const std::array<std::uint8_t, SlottedPage::PAGE_SIZE>&
BufferPool::PageGuard::bytes() const {
    if (!frame_) throw std::runtime_error("Invalid page guard");
    return frame_->bytes;
}

void BufferPool::PageGuard::markDirty() {
    if (!frame_) throw std::runtime_error("Invalid page guard");
    frame_->dirty = true;
}

void BufferPool::PageGuard::release() {
    if (pool_ && frame_) pool_->unpin(frame_);
    pool_ = nullptr;
    frame_ = nullptr;
}

BufferPool::BufferPool(std::size_t capacityPages)
    : capacityPages_(std::max<std::size_t>(1, capacityPages)) {}

BufferPool::~BufferPool() {
    try {
        flushAll();
    } catch (...) {
    }
}

BufferPool::PageGuard BufferPool::fetch(
    const std::filesystem::path& file,
    std::uint32_t pageNumber,
    bool create) {
    PageKey key{canonicalPath(file), pageNumber};
    auto found = frames_.find(key);
    if (found != frames_.end()) {
        auto* frame = found->second.get();
        ++frame->pins;
        touch(frame);
        return PageGuard(this, frame);
    }

    while (frames_.size() >= capacityPages_) evictOne();
    auto frame = std::make_unique<Frame>();
    frame->key = key;
    readPage(key, frame->bytes, create);
    frame->pins = 1;
    lru_.push_front(key);
    frame->lru = lru_.begin();
    auto* pointer = frame.get();
    frames_.emplace(std::move(key), std::move(frame));
    return PageGuard(this, pointer);
}

void BufferPool::flushAll() {
    for (auto& item : frames_) flush(*item.second);
}

void BufferPool::discardAll() {
    for (const auto& item : frames_) {
        if (item.second->pins != 0) {
            throw std::runtime_error(
                "Cannot discard buffer pool with pinned pages");
        }
    }
    frames_.clear();
    lru_.clear();
}

void BufferPool::flushFile(const std::filesystem::path& file) {
    const std::string target = canonicalPath(file);
    for (auto& item : frames_) {
        if (item.first.file == target) flush(*item.second);
    }
}

void BufferPool::invalidateFile(const std::filesystem::path& file) {
    const std::string target = canonicalPath(file);
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (it->first.file != target) {
            ++it;
            continue;
        }
        if (it->second->pins != 0) {
            throw std::runtime_error("Cannot invalidate pinned page");
        }
        lru_.erase(it->second->lru);
        it = frames_.erase(it);
    }
}

std::size_t BufferPool::residentPages() const {
    return frames_.size();
}

std::size_t BufferPool::PageKeyHash::operator()(
    const PageKey& key) const {
    return combineHash(std::hash<std::string>{}(key.file),
                       std::hash<std::uint32_t>{}(key.page));
}

std::string BufferPool::canonicalPath(
    const std::filesystem::path& path) {
    if (path.is_absolute()) return path.lexically_normal().string();
    return std::filesystem::absolute(path).lexically_normal().string();
}

void BufferPool::unpin(Frame* frame) {
    if (frame->pins == 0) {
        throw std::runtime_error("Buffer pool pin underflow");
    }
    --frame->pins;
}

void BufferPool::touch(Frame* frame) {
    lru_.splice(lru_.begin(), lru_, frame->lru);
    frame->lru = lru_.begin();
}

void BufferPool::evictOne() {
    for (auto it = lru_.rbegin(); it != lru_.rend(); ++it) {
        auto found = frames_.find(*it);
        if (found != frames_.end() && found->second->pins == 0) {
            flush(*found->second);
            lru_.erase(found->second->lru);
            frames_.erase(found);
            return;
        }
    }
    throw std::runtime_error("All buffer pool pages are pinned");
}

void BufferPool::flush(Frame& frame) {
    if (!frame.dirty) return;
    writePage(frame.key, frame.bytes);
    frame.dirty = false;
}

void BufferPool::readPage(
    const PageKey& key,
    std::array<std::uint8_t, SlottedPage::PAGE_SIZE>& bytes,
    bool create) {
    bytes.fill(0);
    std::ifstream input(key.file, std::ios::binary);
    if (!input.is_open()) {
        if (!create) {
            throw std::runtime_error("Cannot open heap file: " + key.file);
        }
        return;
    }
    input.seekg(static_cast<std::streamoff>(key.page) *
                static_cast<std::streamoff>(SlottedPage::PAGE_SIZE));
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != 0 &&
        input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("Partial page in heap file: " + key.file);
    }
}

void BufferPool::writePage(
    const PageKey& key,
    const std::array<std::uint8_t, SlottedPage::PAGE_SIZE>& bytes) {
    std::filesystem::create_directories(
        std::filesystem::path(key.file).parent_path());
    std::fstream output(key.file,
                        std::ios::binary | std::ios::in | std::ios::out);
    if (!output.is_open()) {
        std::ofstream create(key.file, std::ios::binary);
        create.close();
        output.open(key.file,
                    std::ios::binary | std::ios::in | std::ios::out);
    }
    if (!output.is_open()) {
        throw std::runtime_error("Cannot write heap file: " + key.file);
    }
    output.seekp(static_cast<std::streamoff>(key.page) *
                 static_cast<std::streamoff>(SlottedPage::PAGE_SIZE));
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("Failed writing heap page: " + key.file);
    }
}

HeapFile::HeapFile(std::filesystem::path path, BufferPool& bufferPool)
    : path_(path.is_absolute()
                ? std::move(path)
                : std::filesystem::absolute(path)),
      bufferPool_(bufferPool) {}

void HeapFile::create() {
    std::filesystem::create_directories(path_.parent_path());
    if (!std::filesystem::exists(path_)) {
        std::ofstream output(path_, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Cannot create heap file: " +
                                     path_.string());
        }
        knownPageCount_ = 0;
    }
}

void HeapFile::drop() {
    bufferPool_.flushFile(path_);
    bufferPool_.invalidateFile(path_);
    std::error_code error;
    std::filesystem::remove(path_, error);
    if (error) throw std::runtime_error(error.message());
    knownPageCount_ = 0;
    insertPageHint_.reset();
}

bool HeapFile::exists() const {
    return std::filesystem::exists(path_);
}

std::size_t HeapFile::pageCount() const {
    if (knownPageCount_) return *knownPageCount_;
    if (!exists()) {
        knownPageCount_ = 0;
        return 0;
    }
    const auto size = std::filesystem::file_size(path_);
    knownPageCount_ = static_cast<std::size_t>(
        (size + SlottedPage::PAGE_SIZE - 1) / SlottedPage::PAGE_SIZE);
    return *knownPageCount_;
}

RowId HeapFile::insert(const Tuple& tuple) {
    create();
    const auto encoded = encodeTuple(tuple);
    if (encoded.size() + 22 > SlottedPage::PAGE_SIZE) {
        throw std::runtime_error("Tuple exceeds page size");
    }

    std::size_t pages = pageCount();
    std::uint32_t firstPage = 0;
    if (insertPageHint_ && *insertPageHint_ < pages) {
        firstPage = *insertPageHint_;
    } else if (pages > 0) {
        firstPage = static_cast<std::uint32_t>(pages - 1);
    }
    for (std::uint32_t page = firstPage; page < pages; ++page) {
        auto guard = bufferPool_.fetch(path_, page, false);
        SlottedPage slotted(guard.bytes());
        if (!slotted.valid()) slotted.initialize();
        auto slot = slotted.insert(encoded);
        if (slot) {
            guard.markDirty();
            insertPageHint_ = page;
            return RowId{page, *slot};
        }
    }

    const auto page = static_cast<std::uint32_t>(pages);
    auto guard = bufferPool_.fetch(path_, page, true);
    SlottedPage slotted(guard.bytes());
    slotted.initialize();
    auto slot = slotted.insert(encoded);
    if (!slot) throw std::runtime_error("Could not insert tuple");
    guard.markDirty();
    bufferPool_.flushFile(path_);
    insertPageHint_ = page;
    knownPageCount_ = pages + 1;
    return RowId{page, *slot};
}

std::vector<StoredRow> HeapFile::scan() {
    std::vector<StoredRow> rows;
    scanBatches(1024, [&](std::vector<StoredRow>&& batch) {
        rows.insert(rows.end(),
                    std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
    });
    return rows;
}

void HeapFile::scanBatches(
    std::size_t batchSize,
    const std::function<void(std::vector<StoredRow>&&)>& visitor) {
    if (batchSize == 0) {
        throw std::runtime_error("Heap scan batch size must be positive");
    }
    std::vector<StoredRow> rows;
    rows.reserve(batchSize);
    const std::size_t pages = pageCount();
    for (std::uint32_t page = 0; page < pages; ++page) {
        auto guard = bufferPool_.fetch(path_, page, false);
        SlottedPage slotted(guard.bytes());
        if (!slotted.valid()) continue;
        for (std::uint16_t slot = 0; slot < slotted.slotCount(); ++slot) {
            auto bytes = slotted.read(slot);
            if (!bytes) continue;
            rows.push_back({RowId{page, slot}, decodeTuple(*bytes)});
            if (rows.size() == batchSize) {
                visitor(std::move(rows));
                rows.clear();
                rows.reserve(batchSize);
            }
        }
    }
    if (!rows.empty()) visitor(std::move(rows));
}

void HeapFile::scanProjectedBatches(
    const std::vector<std::size_t>& columns,
    std::size_t batchSize,
    const std::function<void(std::vector<StoredRow>&&)>& visitor) {
    if (batchSize == 0) {
        throw std::runtime_error("Heap scan batch size must be positive");
    }
    if (!std::is_sorted(columns.begin(), columns.end()) ||
        std::adjacent_find(columns.begin(), columns.end()) !=
            columns.end()) {
        throw std::runtime_error(
            "Projected heap columns must be sorted and unique");
    }
    std::vector<StoredRow> rows;
    rows.reserve(batchSize);
    const std::size_t pages = pageCount();
    for (std::uint32_t page = 0; page < pages; ++page) {
        auto guard = bufferPool_.fetch(path_, page, false);
        SlottedPage slotted(guard.bytes());
        if (!slotted.valid()) continue;
        for (std::uint16_t slot = 0; slot < slotted.slotCount(); ++slot) {
            auto bytes = slotted.read(slot);
            if (!bytes) continue;
            rows.push_back({
                RowId{page, slot},
                decodeTupleProjected(*bytes, columns)});
            if (rows.size() == batchSize) {
                visitor(std::move(rows));
                rows.clear();
                rows.reserve(batchSize);
            }
        }
    }
    if (!rows.empty()) visitor(std::move(rows));
}

std::optional<StoredRow> HeapFile::read(RowId row) {
    auto guard = bufferPool_.fetch(path_, row.page, false);
    SlottedPage slotted(guard.bytes());
    auto bytes = slotted.read(row.slot);
    if (!bytes) return std::nullopt;
    return StoredRow{row, decodeTuple(*bytes)};
}

bool HeapFile::erase(RowId row) {
    if (row.page >= pageCount()) return false;
    auto guard = bufferPool_.fetch(path_, row.page, false);
    SlottedPage slotted(guard.bytes());
    if (!slotted.erase(row.slot)) return false;
    guard.markDirty();
    return true;
}

RowId HeapFile::update(RowId row, const Tuple& tuple) {
    const auto encoded = encodeTuple(tuple);
    if (row.page < pageCount()) {
        auto guard = bufferPool_.fetch(path_, row.page, false);
        SlottedPage slotted(guard.bytes());
        if (slotted.update(row.slot, encoded)) {
            guard.markDirty();
            return row;
        }
    }
    if (!erase(row)) throw std::runtime_error("Row no longer exists");
    return insert(tuple);
}

void HeapFile::flush() {
    bufferPool_.flushFile(path_);
}

std::vector<std::uint8_t> HeapFile::encodeTuple(const Tuple& tuple) {
    if (tuple.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("Too many tuple fields");
    }
    std::vector<std::uint8_t> out;
    appendPod(out, static_cast<std::uint16_t>(tuple.size()));
    for (const auto& value : tuple) {
        out.push_back(static_cast<std::uint8_t>(value.type()));
        switch (value.type()) {
            case ValueType::Null:
                break;
            case ValueType::Integer: {
                const auto integer = value.asInteger();
                appendPod(out, integer);
                break;
            }
            case ValueType::Real: {
                const auto real = value.asReal();
                appendPod(out, real);
                break;
            }
            case ValueType::Boolean:
                out.push_back(value.asBoolean() ? 1 : 0);
                break;
            case ValueType::Text: {
                const auto& text = value.asText();
                appendPod(out, static_cast<std::uint32_t>(text.size()));
                out.insert(out.end(), text.begin(), text.end());
                break;
            }
            case ValueType::Blob: {
                const auto& blob = value.asBlob();
                appendPod(out, static_cast<std::uint32_t>(blob.size()));
                out.insert(out.end(), blob.begin(), blob.end());
                break;
            }
        }
    }
    return out;
}

Tuple HeapFile::decodeTuple(const std::vector<std::uint8_t>& bytes) {
    std::size_t offset = 0;
    const std::uint16_t count =
        readPod<std::uint16_t>(bytes, offset);
    std::vector<std::size_t> columns(count);
    for (std::size_t index = 0; index < count; ++index) {
        columns[index] = index;
    }
    return decodeTupleProjected(bytes, columns);
}

Tuple HeapFile::decodeTupleProjected(
    const std::vector<std::uint8_t>& bytes,
    const std::vector<std::size_t>& columns) {
    std::size_t offset = 0;
    const std::uint16_t count =
        readPod<std::uint16_t>(bytes, offset);
    if (!columns.empty() &&
        (columns.back() >= count ||
         !std::is_sorted(columns.begin(), columns.end()) ||
         std::adjacent_find(columns.begin(), columns.end()) !=
             columns.end())) {
        throw std::runtime_error("Invalid projected tuple columns");
    }
    Tuple tuple;
    tuple.reserve(columns.size());
    std::size_t projected = 0;
    for (std::uint16_t index = 0; index < count; ++index) {
        if (offset >= bytes.size()) {
            throw std::runtime_error("Corrupt tuple type tag");
        }
        const auto type = static_cast<ValueType>(bytes[offset++]);
        const bool materialize =
            projected < columns.size() &&
            columns[projected] == index;
        switch (type) {
            case ValueType::Null:
                if (materialize) tuple.push_back(Value::null());
                break;
            case ValueType::Integer: {
                const auto value =
                    readPod<std::int64_t>(bytes, offset);
                if (materialize) tuple.emplace_back(value);
                break;
            }
            case ValueType::Real: {
                const auto value = readPod<double>(bytes, offset);
                if (materialize) tuple.emplace_back(value);
                break;
            }
            case ValueType::Boolean:
                if (offset >= bytes.size()) {
                    throw std::runtime_error("Corrupt boolean tuple field");
                }
                if (materialize) {
                    tuple.emplace_back(bytes[offset] != 0);
                }
                ++offset;
                break;
            case ValueType::Text: {
                const auto length =
                    readPod<std::uint32_t>(bytes, offset);
                if (offset + length > bytes.size()) {
                    throw std::runtime_error("Corrupt text tuple field");
                }
                if (materialize) {
                    tuple.emplace_back(std::string(
                        reinterpret_cast<const char*>(
                            bytes.data() + offset),
                        length));
                }
                offset += length;
                break;
            }
            case ValueType::Blob: {
                const auto length =
                    readPod<std::uint32_t>(bytes, offset);
                if (offset + length > bytes.size()) {
                    throw std::runtime_error("Corrupt blob tuple field");
                }
                if (materialize) {
                    Value::Blob blob(bytes.begin() + offset,
                                     bytes.begin() + offset + length);
                    tuple.emplace_back(std::move(blob));
                }
                offset += length;
                break;
            }
            default:
                throw std::runtime_error("Unknown tuple field type");
        }
        if (materialize && !columns.empty()) ++projected;
    }
    return tuple;
}
