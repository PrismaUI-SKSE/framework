#pragma once

#include <Windows.h>

#include <map>
#include <memory>
#include <shared_mutex>

#include "Cef/Browser/CefRuntime.h"
#include "ImeHelper.h"

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
    struct PrismaView;
}

namespace PrismaUI {
    class InputHandler : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputHandler& GetSingleton();

        bool Initialize(HWND gameHwnd, std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                        std::shared_mutex* viewsMapMutex);

        void EnableInputCapture(Core::PrismaViewId viewId);
        void DisableInputCapture(Core::PrismaViewId viewId);
        void ClearImeState(Core::PrismaViewId viewId);
        bool IsAnyInputCaptureActive() const;
        void ProcessEvents();
        void Shutdown();
        void OnExit(std::move_only_function<void()>&& callback);

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
                                              RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

    private:
        static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                                             DWORD_PTR dwRefData);

        void InstallWndProcHook();
        bool InstallWndProcHookAttempt();
        void UninstallWndProcHook() const;

        std::string GetClipboardText() const;
        void SetClipboardText(const std::string& text) const;

        void QueueInputEvent(Cef::CefInputEvent event);
        uint32_t GetMouseModifiers() const;
        void QueueCommittedCharEvent(const std::wstring& utf16Text, LPARAM lParam);

        HWND _hWnd{};
        std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* _viewsMap{};
        std::shared_mutex* _viewsMapMutex{};
        ImeHelper _imeHelper;
        Core::PrismaViewId _currentlyFocusedViewId{};
        std::mutex _focusedViewIdMutex;
        std::mutex _eventQueueMutex;
        std::vector<Cef::CefInputEvent> _eventQueue;
        std::atomic<bool> _isAnyInputCaptureActive = false;
        std::atomic<bool> _isFocusedTextInputActive = false;
        // What the game keeps seeing in RE::MenuCursor while a view is focused: the position the cursor had
        // when focus started. Vanilla menus underneath hit-test MenuCursor every frame, so freezing it is
        // what keeps them inert; an off-screen position would make MapMenu edge-scroll to the corner.
        float _freezedCursorX = 0.0f;
        float _freezedCursorY = 0.0f;
        // The real cursor position PrismaUI tracks for CEF and for the drawn arrow. See ProcessEvent.
        float _cursorX = 0.0f;
        float _cursorY = 0.0f;
        std::move_only_function<void()> _onExitCallback{};
        bool _mouseButtonStates[3] = {false, false, false};
        wchar_t _pendingHighSurrogate = 0;
        bool _isInitialized = false;
    };
}
