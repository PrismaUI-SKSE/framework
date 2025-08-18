#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <utility>
#include <chrono>
#include <iostream>

class RepeatingTaskRunner {
public:
    explicit RepeatingTaskRunner(std::function<void()> task)
        : stop_flag_(false),
        repeating_task_(std::move(task))
    {
        if (!repeating_task_) {
            throw std::invalid_argument("RepeatingTaskRunner: Provided task is empty.");
        }
        worker_thread_ = std::thread(&RepeatingTaskRunner::run, this);
    }

    ~RepeatingTaskRunner() {
        stop();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    RepeatingTaskRunner(const RepeatingTaskRunner&) = delete;
    RepeatingTaskRunner& operator=(const RepeatingTaskRunner&) = delete;
    RepeatingTaskRunner(RepeatingTaskRunner&&) = delete;
    RepeatingTaskRunner& operator=(RepeatingTaskRunner&&) = delete;

    void stop() {
        stop_flag_.store(true, std::memory_order_release);
    }

private:
    void run() {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            try {
                repeating_task_();

                std::this_thread::sleep_for(std::chrono::milliseconds(1));

            }
            catch (const std::exception& e) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            catch (...) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    std::thread worker_thread_;
    std::atomic<bool> stop_flag_;
    std::function<void()> repeating_task_;
};
