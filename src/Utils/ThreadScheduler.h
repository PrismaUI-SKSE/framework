#pragma once
#include <functional>
#include <queue>
#include <thread>

#include "ResourceLock.h"

class ThreadScheduler {
public:
    void SetThreadId(std::thread::id threadId) { _threadId = threadId; }

    bool IsTargetThread() const { return std::this_thread::get_id() == _threadId; }

    template <std::invocable TTask>
    void Post(TTask&& task) {
        if (IsTargetThread()) {
            std::invoke(std::forward<TTask>(task));
        } else {
            auto taskQueue = _taskQueueLock.Acquire();
            taskQueue->push(std::forward<TTask>(task));
        }
    }

    void ExecuteTasks() {
        std::queue<std::move_only_function<void()>> tasks;

        {
            auto taskQueue = _taskQueueLock.Acquire();
            if (taskQueue->empty()) {
                return;
            }

            tasks.swap(*taskQueue);
        }

        while (!tasks.empty()) {
            std::invoke(std::move(tasks.front()));
            tasks.pop();
        }
    }

private:
    ResourceLock<std::queue<std::move_only_function<void()>>> _taskQueueLock{
        std::queue<std::move_only_function<void()>>()};
    std::thread::id _threadId{};
};
