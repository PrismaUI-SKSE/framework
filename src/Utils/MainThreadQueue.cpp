#include "Utils/MainThreadQueue.h"

#include <mutex>
#include <utility>
#include <vector>

namespace PrismaUI::MainThreadQueue {
    namespace {
        std::mutex g_queueMutex;
        std::vector<std::function<void()>> g_queued;

        class PumpEventSink : public RE::BSTEventSink<RE::InputEvent*> {
        public:
            static PumpEventSink* GetSingleton() {
                static PumpEventSink singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* /*a_event*/,
                RE::BSTEventSource<RE::InputEvent*>* /*a_eventSource*/) override {
                Drain();
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Initialize() {
        if (auto* inputDeviceManager = RE::BSInputDeviceManager::GetSingleton()) {
            inputDeviceManager->AddEventSink(PumpEventSink::GetSingleton());
            logger::info("MainThreadQueue pump registered with BSInputDeviceManager");
        } else {
            logger::error("Failed to register MainThreadQueue pump: BSInputDeviceManager is null");
        }
    }

    void Shutdown() {
        if (auto* inputDeviceManager = RE::BSInputDeviceManager::GetSingleton()) {
            inputDeviceManager->RemoveEventSink(PumpEventSink::GetSingleton());
        }
        Clear();
    }

    void Post(std::function<void()> task) {
        if (!task) {
            return;
        }
        std::lock_guard lock(g_queueMutex);
        g_queued.push_back(std::move(task));
    }

    void Drain() {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard lock(g_queueMutex);
            if (g_queued.empty()) {
                return;
            }
            pending.swap(g_queued);
        }

        for (auto& task : pending) {
            try {
                task();
            } catch (const std::exception& e) {
                logger::error("MainThreadQueue: exception in queued task: {}", e.what());
            } catch (...) {
                logger::error("MainThreadQueue: unknown exception in queued task");
            }
        }
    }

    void Clear() {
        std::lock_guard lock(g_queueMutex);
        g_queued.clear();
    }
}
