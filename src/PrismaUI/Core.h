#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>

#include "Menus/FocusMenu/FocusMenu.h"

namespace PRISMA_UI_API {
    enum class ConsoleMessageLevel : uint8_t;
}

namespace PrismaUI::Core {

    typedef uint64_t PrismaViewId;

    struct PrismaView {
        PrismaViewId id;
        // CEF shell iframe name and resolved URL.
        std::string iframeName;
        std::string resolvedUrl;
        std::string originalUrl;
        // Lifecycle/state atomics owned by the native CEF shell adapter.
        std::atomic<bool> isHidden = false;
        std::atomic<bool> isFocused = false;
        std::atomic<bool> iframeCreateRequested = false;
        std::atomic<bool> iframeReady = false;  // Set by CEF shell load callbacks (wired in Step 5/7).
        std::atomic<bool> destroyRequested = false;
        std::atomic<bool> isPaused = false;
        int scrollingPixelSize = 28;
        int order = 0;
        std::function<void(PrismaViewId)> domReadyCallback;
        std::function<void(PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)>
            consoleMessageCallback;

        // Operation queue fields for thread-safe sequential execution
        std::mutex operationMutex;
        std::queue<std::function<void()>> pendingOperations;
        std::atomic<bool> isProcessingOperation = false;
        std::atomic<int> queuedOperationsCount = 0;
    };

    extern std::atomic_uint64_t nextViewId;

    extern std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
    extern std::shared_mutex viewsMutex;

    using SimpleJSCallback = std::function<void(const std::string&)>;

    struct JSCallbackData {
        PrismaViewId viewId;
        std::string name;
        SimpleJSCallback callback;
    };

    extern std::map<std::pair<PrismaViewId, std::string>, JSCallbackData> jsCallbacks;
    extern std::mutex jsCallbacksMutex;

    void Shutdown();
}
