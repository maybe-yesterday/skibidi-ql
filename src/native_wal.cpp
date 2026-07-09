#include "native_wal.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

constexpr char kBegin = 'B';
constexpr char kFileBefore = 'F';
constexpr char kCommit = 'C';

template <typename T>
void writePod(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value),
                 static_cast<std::streamsize>(sizeof(T)));
}

template <typename T>
bool readPod(std::ifstream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value),
               static_cast<std::streamsize>(sizeof(T)));
    return static_cast<bool>(input);
}

void restoreFile(const std::filesystem::path& path,
                 const WalFileImage& image) {
    if (!image.existed) {
        std::error_code error;
        std::filesystem::remove(path, error);
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "Cannot restore WAL file image: " + path.string());
    }
    if (!image.bytes.empty()) {
        output.write(reinterpret_cast<const char*>(image.bytes.data()),
                     static_cast<std::streamsize>(image.bytes.size()));
    }
    output.flush();
    if (!output) {
        throw std::runtime_error(
            "Failed writing WAL file image: " + path.string());
    }
}

} // namespace

NativeWal::NativeWal(std::filesystem::path root)
    : root_(std::move(root)) {}

void NativeWal::resetRoot(std::filesystem::path root) {
    root_ = std::move(root);
}

std::filesystem::path NativeWal::walPath() const {
    return root_ / "skibidi.wal";
}

std::string NativeWal::relativePath(
    const std::filesystem::path& path) const {
    const auto absoluteRoot =
        std::filesystem::absolute(root_).lexically_normal();
    const auto absolutePath =
        std::filesystem::absolute(path).lexically_normal();
    return std::filesystem::relative(absolutePath, absoluteRoot)
        .generic_string();
}

std::uint64_t NativeWal::begin() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch().count());
    const auto transactionId = stamp ^ sequence.fetch_add(
        1, std::memory_order_relaxed);
    appendRecord(kBegin, transactionId, {}, false, {});
    return transactionId;
}

void NativeWal::logBefore(
    std::uint64_t transactionId,
    const std::filesystem::path& path,
    const std::optional<std::vector<std::uint8_t>>& before) {
    appendRecord(kFileBefore, transactionId, relativePath(path),
                 before.has_value(),
                 before.value_or(std::vector<std::uint8_t>{}));
}

void NativeWal::commit(std::uint64_t transactionId) {
    appendRecord(kCommit, transactionId, {}, false, {});
}

void NativeWal::checkpoint() {
    std::error_code error;
    std::filesystem::remove(walPath(), error);
}

void NativeWal::appendRecord(char type,
                             std::uint64_t transactionId,
                             const std::string& relativePathValue,
                             bool existed,
                             const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(root_);
    std::ofstream output(walPath(),
                         std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("Cannot open WAL: " +
                                 walPath().string());
    }
    const std::uint32_t pathSize =
        static_cast<std::uint32_t>(relativePathValue.size());
    const std::uint64_t dataSize =
        static_cast<std::uint64_t>(bytes.size());
    const std::uint8_t existedByte = existed ? 1 : 0;

    output.write(&type, 1);
    writePod(output, transactionId);
    writePod(output, pathSize);
    writePod(output, dataSize);
    writePod(output, existedByte);
    output.write(relativePathValue.data(),
                 static_cast<std::streamsize>(relativePathValue.size()));
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    if (!output) throw std::runtime_error("Failed writing WAL record");
}

void NativeWal::recover() {
    const auto path = walPath();
    if (!std::filesystem::exists(path)) return;

    struct Transaction {
        bool committed = false;
        std::map<std::string, WalFileImage> beforeImages;
    };
    std::map<std::uint64_t, Transaction> transactions;

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read WAL: " + path.string());
    }
    while (true) {
        char type = 0;
        input.read(&type, 1);
        if (!input) break;

        std::uint64_t transactionId = 0;
        std::uint32_t pathSize = 0;
        std::uint64_t dataSize = 0;
        std::uint8_t existedByte = 0;
        if (!readPod(input, transactionId) ||
            !readPod(input, pathSize) ||
            !readPod(input, dataSize) ||
            !readPod(input, existedByte)) {
            break;
        }

        std::string relative(pathSize, '\0');
        if (pathSize > 0) {
            input.read(relative.data(),
                       static_cast<std::streamsize>(relative.size()));
            if (!input) break;
        }
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(dataSize));
        if (dataSize > 0) {
            input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!input) break;
        }

        auto& transaction = transactions[transactionId];
        if (type == kCommit) {
            transaction.committed = true;
        } else if (type == kFileBefore && !relative.empty()) {
            transaction.beforeImages.emplace(
                relative,
                WalFileImage{existedByte != 0, std::move(bytes)});
        }
    }
    input.close();

    for (const auto& entry : transactions) {
        const auto& transaction = entry.second;
        if (transaction.committed) continue;
        for (auto file = transaction.beforeImages.rbegin();
             file != transaction.beforeImages.rend();
             ++file) {
            restoreFile(root_ / std::filesystem::path(file->first),
                        file->second);
        }
    }

    checkpoint();
}
