#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

enum class NativeLockMode {
    Shared,
    Exclusive
};

struct NativeLockRequest {
    std::string resource;
    NativeLockMode mode = NativeLockMode::Shared;
};

class NativeLockGuard {
public:
    NativeLockGuard() = default;
    NativeLockGuard(std::shared_ptr<std::shared_mutex> mutex,
                    NativeLockMode mode);
    NativeLockGuard(const NativeLockGuard&) = delete;
    NativeLockGuard& operator=(const NativeLockGuard&) = delete;
    NativeLockGuard(NativeLockGuard&& other) noexcept;
    NativeLockGuard& operator=(NativeLockGuard&& other) noexcept;
    ~NativeLockGuard();

    void unlock();
    explicit operator bool() const { return mutex_ != nullptr; }

private:
    std::shared_ptr<std::shared_mutex> mutex_;
    NativeLockMode mode_ = NativeLockMode::Shared;
    bool locked_ = false;
};

// Process-local lock registry used by NativeEngine to make one engine safe to
// call from multiple threads and to coordinate multiple NativeEngine objects in
// the same process. It is intentionally not an OS file lock; cross-process
// locking is a separate durability layer.
class NativeLockManager {
public:
    static NativeLockManager& global();

    NativeLockGuard acquire(const std::string& resource,
                            NativeLockMode mode);
    std::vector<NativeLockGuard> acquireAll(
        std::vector<NativeLockRequest> requests);

private:
    std::shared_ptr<std::shared_mutex> mutexFor(
        const std::string& resource);
};
