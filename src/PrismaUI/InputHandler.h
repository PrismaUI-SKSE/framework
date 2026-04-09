#pragma once

#include "Utils/WinKeyHandler/WinKeyHandler.h"

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

#include <map>
#include <memory>
#include <shared_mutex>
#include <variant>

class SingleThreadExecutor;

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
    struct PrismaView;
}

namespace PrismaUI::InputHandler {
    using namespace ultralight;

    // Wrapper for scroll events that includes mouse position for proper routing
    struct ScrollEventWithPosition {
        ScrollEvent event;
        int mouseX;
        int mouseY;
    };

    using InputEvent = std::variant<MouseEvent, ScrollEventWithPosition, KeyEvent>;

    void Initialize(HWND gameHwnd, SingleThreadExecutor* coreExecutor,
                    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                    std::shared_mutex* viewsMapMutex);

    void EnableInputCapture(const Core::PrismaViewId& viewId);
    void DisableInputCapture(const Core::PrismaViewId& viewId);
    void ClearImeState(const Core::PrismaViewId& viewId);

    bool IsInputCaptureActiveForView(const Core::PrismaViewId& viewId);

    bool IsAnyInputCaptureActive();

    bool InstallWndProcHook();
    void UninstallWndProcHook();

    void ProcessEvents();
    void Shutdown();
}
