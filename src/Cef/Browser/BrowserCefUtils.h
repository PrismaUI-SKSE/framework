#pragma once

#include "cef_task.h"

namespace PrismaUI::Cef {
    template <std::invocable TTask>
    class FunctionTask final : public CefTask {
    public:
        explicit FunctionTask(TTask&& task) : _task(std::forward<TTask>(task)) {}

        void Execute() override {
            try {
                std::invoke(_task);
            } catch (const std::exception& e) {
                logger::error("Exception in CEF UI task: {}", e.what());
            } catch (...) {
                logger::error("Unknown exception in CEF UI task.");
            }
        }

    private:
        TTask _task;

        IMPLEMENT_REFCOUNTING(FunctionTask);
    };

    template <std::invocable TTask>
    void PostToCefUi(TTask&& task) {
        if (CefCurrentlyOn(TID_UI)) {
            task();
            return;
        }

        if (!CefPostTask(TID_UI, new FunctionTask(std::forward<TTask>(task)))) {
            logger::error("Failed to post task to CEF UI thread.");
        }
    }
}
