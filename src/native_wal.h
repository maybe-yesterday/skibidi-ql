#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct WalFileImage {
    bool existed = false;
    std::vector<std::uint8_t> bytes;
};

// Minimal undo WAL for the native heap/catalog files. Mutating statements log
// each file's before-image once, flush the data, then append a commit marker.
// Startup recovery replays only uncommitted before-images and checkpoints the
// log. This favors correctness and simplicity over fine-grained page logging.
class NativeWal {
public:
    explicit NativeWal(std::filesystem::path root = {});

    void resetRoot(std::filesystem::path root);
    void recover();
    std::uint64_t begin();
    void logBefore(std::uint64_t transactionId,
                   const std::filesystem::path& path,
                   const std::optional<std::vector<std::uint8_t>>& before);
    void commit(std::uint64_t transactionId);
    void checkpoint();

    std::filesystem::path walPath() const;

private:
    std::filesystem::path root_;

    std::string relativePath(const std::filesystem::path& path) const;
    void appendRecord(char type,
                      std::uint64_t transactionId,
                      const std::string& relativePath,
                      bool existed,
                      const std::vector<std::uint8_t>& bytes);
};
