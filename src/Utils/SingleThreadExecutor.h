#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <future>
#include <stdexcept>
#include <utility>
#include <type_traits>

class SingleThreadExecutor {
public:
    SingleThreadExecutor();
    ~SingleThreadExecutor();

    SingleThreadExecutor(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor& operator=(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor(SingleThreadExecutor&&) = delete;
    SingleThreadExecutor& operator=(SingleThreadExecutor&&) = delete;

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task_ptr = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<ReturnType> res = task_ptr->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("Executor is stopping");
            }
            tasks_.emplace([task_ptr]() { (*task_ptr)(); });
        }
        condition_.notify_one();
        return res;
    }

private:
    void run();

    std::thread worker_thread_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
};

inline SingleThreadExecutor::SingleThreadExecutor() : stop_(false) {
    worker_thread_ = std::thread(&SingleThreadExecutor::run, this);
}

inline SingleThreadExecutor::~SingleThreadExecutor() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_one();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

inline void SingleThreadExecutor::run() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        }
        catch (...) {}
    }
}

