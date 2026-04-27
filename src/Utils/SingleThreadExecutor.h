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
#include <vector>
#include <algorithm>

class SingleThreadExecutor {
public:
    enum class Priority {
        HIGH = 0,    // Input events - immediate processing
        MEDIUM = 1,  // Inspector updates - debugging tool
        LOW = 2      // Primary view rendering - can tolerate delays
    };

    SingleThreadExecutor();
    ~SingleThreadExecutor();

    SingleThreadExecutor(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor& operator=(const SingleThreadExecutor&) = delete;
    SingleThreadExecutor(SingleThreadExecutor&&) = delete;
    SingleThreadExecutor& operator=(SingleThreadExecutor&&) = delete;

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
    {
        return submit_with_priority(Priority::LOW, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template<typename F, typename... Args>
    auto submit_with_priority(Priority priority, F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
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
            tasks_.push_back({priority, next_seq_++, [task_ptr]() { (*task_ptr)(); }});
            std::push_heap(tasks_.begin(), tasks_.end(), TaskCompare());
        }
        condition_.notify_one();
        return res;
    }

    bool IsWorkerThread() const {
        return std::this_thread::get_id() == worker_thread_id_.load();
    }

    void SetExceptionHandler(std::function<void(const std::exception_ptr&)> handler) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        exception_handler_ = std::move(handler);
    }

private:
    struct Task {
        Priority priority;
        std::uint64_t seq;
        std::function<void()> func;
    };

    struct TaskCompare {
        bool operator()(const Task& a, const Task& b) const {
            // Lower priority value = higher priority (inverted for max heap).
            // For equal priorities, lower seq runs first → stable FIFO within a priority.
            if (a.priority != b.priority) {
                return static_cast<int>(a.priority) > static_cast<int>(b.priority);
            }
            return a.seq > b.seq;
        }
    };

    void run();

    std::thread worker_thread_;
    std::atomic<std::thread::id> worker_thread_id_;
    std::vector<Task> tasks_;  // Using vector as heap for priority queue
    std::uint64_t next_seq_;   // Monotonic submission counter; tiebreaker for equal priorities (guarded by queue_mutex_)
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_;
    std::function<void(const std::exception_ptr&)> exception_handler_;
};

inline SingleThreadExecutor::SingleThreadExecutor() : worker_thread_id_(), next_seq_(0), stop_(false), exception_handler_(nullptr) {
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
    // Set thread ID from within the worker thread to avoid race condition
    worker_thread_id_.store(std::this_thread::get_id());
    
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) {
                return;
            }
            // Pop highest priority task from heap
            std::pop_heap(tasks_.begin(), tasks_.end(), TaskCompare());
            task = std::move(tasks_.back().func);
            tasks_.pop_back();
        }
        try {
            task();
        }
        catch (...) {
            // Infrastructure exceptions (not user task exceptions, which are
            // captured by packaged_task). Report via handler if available.
            std::function<void(const std::exception_ptr&)> handler;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                handler = exception_handler_;
            }
            if (handler) {
                try {
                    handler(std::current_exception());
                } catch (...) {
                    // Prevent exception handler from throwing
                }
            }
        }
    }
}

