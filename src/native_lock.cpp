#include "native_lock.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace {

std::mutex& registryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::shared_ptr<std::shared_mutex>>&
registry() {
    static std::unordered_map<std::string,
                              std::shared_ptr<std::shared_mutex>> locks;
    return locks;
}

} // namespace

NativeLockGuard::NativeLockGuard(std::shared_ptr<std::shared_mutex> mutex,
                                 NativeLockMode mode)
    : mutex_(std::move(mutex)), mode_(mode) {
    if (!mutex_) return;
    if (mode_ == NativeLockMode::Exclusive) mutex_->lock();
    else mutex_->lock_shared();
    locked_ = true;
}

NativeLockGuard::NativeLockGuard(NativeLockGuard&& other) noexcept {
    *this = std::move(other);
}

NativeLockGuard& NativeLockGuard::operator=(
    NativeLockGuard&& other) noexcept {
    if (this == &other) return *this;
    unlock();
    mutex_ = std::move(other.mutex_);
    mode_ = other.mode_;
    locked_ = other.locked_;
    other.locked_ = false;
    return *this;
}

NativeLockGuard::~NativeLockGuard() {
    unlock();
}

void NativeLockGuard::unlock() {
    if (!locked_ || !mutex_) return;
    if (mode_ == NativeLockMode::Exclusive) mutex_->unlock();
    else mutex_->unlock_shared();
    locked_ = false;
}

NativeLockManager& NativeLockManager::global() {
    static NativeLockManager manager;
    return manager;
}

std::shared_ptr<std::shared_mutex> NativeLockManager::mutexFor(
    const std::string& resource) {
    std::lock_guard<std::mutex> guard(registryMutex());
    auto& locks = registry();
    auto found = locks.find(resource);
    if (found != locks.end()) return found->second;
    auto mutex = std::make_shared<std::shared_mutex>();
    locks.emplace(resource, mutex);
    return mutex;
}

NativeLockGuard NativeLockManager::acquire(const std::string& resource,
                                           NativeLockMode mode) {
    return NativeLockGuard(mutexFor(resource), mode);
}

std::vector<NativeLockGuard> NativeLockManager::acquireAll(
    std::vector<NativeLockRequest> requests) {
    std::sort(requests.begin(), requests.end(),
        [](const NativeLockRequest& left,
           const NativeLockRequest& right) {
        return left.resource < right.resource;
    });

    std::vector<NativeLockRequest> deduped;
    for (const auto& request : requests) {
        if (request.resource.empty()) continue;
        if (!deduped.empty() &&
            deduped.back().resource == request.resource) {
            if (request.mode == NativeLockMode::Exclusive) {
                deduped.back().mode = NativeLockMode::Exclusive;
            }
            continue;
        }
        deduped.push_back(request);
    }

    std::vector<NativeLockGuard> guards;
    guards.reserve(deduped.size());
    for (const auto& request : deduped) {
        guards.push_back(acquire(request.resource, request.mode));
    }
    return guards;
}
