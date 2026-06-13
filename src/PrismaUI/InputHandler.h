#pragma once

#include <Windows.h>

#include <map>
#include <memory>
#include <shared_mutex>

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
    struct PrismaView;
}

namespace PrismaUI::InputHandler {
    bool Initialize(HWND gameHwnd, std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                    std::shared_mutex* viewsMapMutex);

    void EnableInputCapture(Core::PrismaViewId viewId);
    void DisableInputCapture(Core::PrismaViewId viewId);
    void ClearImeState(Core::PrismaViewId viewId);

    bool IsInputCaptureActiveForView(Core::PrismaViewId viewId);

    bool IsAnyInputCaptureActive();

    bool InstallWndProcHook();
    void UninstallWndProcHook();

    void ProcessEvents();
    void Shutdown();
}
