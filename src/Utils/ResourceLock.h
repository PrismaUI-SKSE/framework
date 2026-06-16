#pragma once

#include <mutex>

template <class T>
class ResourceLock {
public:
    class Lock {
    public:
        Lock(std::mutex& mutex, T& value) : guard_(mutex), value_(&value) {}

        T& operator*() const noexcept { return *value_; }
        T* operator->() const noexcept { return value_; }

    private:
        std::lock_guard<std::mutex> guard_;
        T* value_;
    };

    explicit ResourceLock(T value) : value_(std::move(value)) {}

    Lock Acquire() { return Lock(mutex_, value_); }

private:
    std::mutex mutex_;
    T value_;
};
