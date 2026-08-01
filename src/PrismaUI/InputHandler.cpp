#include "InputHandler.h"

#include <commctrl.h>

#include "Cef/Browser/CefRuntime.h"
#include "Cef/Shared/ProcessMessageNames.h"
#include "Communication.h"
#include "Core.h"
#include "ImeHelper.h"
#include "Utils/Encoding.h"
#include "Utils/WinKeyHandler/WinKeyHandler.h"
#include "include/internal/cef_types.h"
#pragma comment(lib, "comctl32.lib")

namespace PrismaUI {
    constexpr int SCROLL_LINES_PER_WHEEL_DELTA = 1;

    // Clipboard safety limits
    constexpr size_t MAX_CLIPBOARD_SIZE = 1024 * 1024;  // 1MB max
    constexpr size_t MAX_CLIPBOARD_CHARS = 200000;      // 200K characters max

    // WndProc subclass state
    static constexpr UINT_PTR SUBCLASS_ID = 0x505249534D41;  // "PRISMA" in hex

    constexpr const char* IME_FOCUS_CALLBACK_NAME = Cef::Messages::kImeFocusListener;

    void RefreshImeFocusTrackingForView(Core::PrismaViewId viewId) {
        if (viewId == 0) {
            return;
        }

        Communication::Invoke(viewId,
                              "if(typeof window.__prismaImeFocusNotify==='function'){"
                              "window.__prismaImeFocusNotify(document.activeElement);}");
    }

    // Clipboard helper functions
    std::string EscapeForJS(const std::string& text) {
        std::string escaped;

        try {
            escaped.reserve(text.size() * 2);  // Reserve extra space for escape sequences
        } catch (const std::exception& e) {
            logger::error("Failed to allocate memory for escaped text: {}", e.what());
            return "";
        }

        for (size_t i = 0; i < text.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(text[i]);

            // Handle multi-byte UTF-8 sequences
            if (c >= 0x80) {
                // Check for Unicode line/paragraph separators (U+2028, U+2029)
                // U+2028 = E2 80 A8, U+2029 = E2 80 A9
                if (i + 2 < text.size() && c == 0xE2 && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                    (static_cast<unsigned char>(text[i + 2]) == 0xA8 ||
                     static_cast<unsigned char>(text[i + 2]) == 0xA9)) {
                    escaped += "\\u202";
                    escaped += (text[i + 2] == 0xA8) ? '8' : '9';
                    i += 2;
                    continue;
                }
                // Pass through other UTF-8 sequences
                escaped += c;
                continue;
            }

            // Handle special characters and control codes
            switch (c) {
                case '\'':
                    escaped += "\\'";
                    break;
                case '\"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                case '\b':
                    escaped += "\\b";
                    break;
                case '\f':
                    escaped += "\\f";
                    break;
                default:
                    // Filter out dangerous control characters (0x00-0x1F except handled above)
                    if (c < 0x20) {
                        // Skip null bytes and other control characters
                        logger::trace("Filtered control character: 0x{:02X}", static_cast<int>(c));
                    } else {
                        escaped += c;
                    }
                    break;
            }
        }
        return escaped;
    }

    std::string InputHandler::GetClipboardText() const {
        if (!OpenClipboard(_hWnd)) {
            logger::warn("Failed to open clipboard for reading");
            return "";
        }

        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (!hData) {
            CloseClipboard();
            return "";
        }

        wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
        if (!pszText) {
            CloseClipboard();
            return "";
        }

        // Check size before converting
        SIZE_T dataSize = GlobalSize(hData);
        if (dataSize > MAX_CLIPBOARD_SIZE) {
            logger::warn("Clipboard text too large: {} bytes (max: {} bytes). Truncating.", dataSize,
                         MAX_CLIPBOARD_SIZE);
            GlobalUnlock(hData);
            CloseClipboard();
            return "";
        }

        // Convert wide string to UTF-8
        int utf8Length = WideCharToMultiByte(CP_UTF8, 0, pszText, -1, nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0) {
            GlobalUnlock(hData);
            CloseClipboard();
            return "";
        }

        // Check character count limit
        size_t charCount = wcslen(pszText);
        if (charCount > MAX_CLIPBOARD_CHARS) {
            logger::warn("Clipboard text too long: {} characters (max: {} characters). Truncating.", charCount,
                         MAX_CLIPBOARD_CHARS);
            GlobalUnlock(hData);
            CloseClipboard();
            return "";
        }

        std::string result;
        try {
            result.resize(utf8Length - 1);
            WideCharToMultiByte(CP_UTF8, 0, pszText, -1, &result[0], utf8Length, nullptr, nullptr);
        } catch (const std::exception& e) {
            logger::error("Failed to allocate memory for clipboard text: {}", e.what());
            GlobalUnlock(hData);
            CloseClipboard();
            return "";
        }

        GlobalUnlock(hData);
        CloseClipboard();

        return result;
    }

    void InputHandler::SetClipboardText(const std::string& text) const {
        // Check size limits before processing
        if (text.size() > MAX_CLIPBOARD_SIZE) {
            logger::warn("Text too large to copy to clipboard: {} bytes (max: {} bytes)", text.size(),
                         MAX_CLIPBOARD_SIZE);
            return;
        }

        if (!OpenClipboard(_hWnd)) {
            logger::warn("Failed to open clipboard for writing");
            return;
        }

        EmptyClipboard();

        // Convert UTF-8 to wide string
        int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (wideLength <= 0) {
            CloseClipboard();
            return;
        }

        HGLOBAL hMem = nullptr;
        try {
            hMem = GlobalAlloc(GMEM_MOVEABLE, wideLength * sizeof(wchar_t));
            if (!hMem) {
                logger::error("Failed to allocate global memory for clipboard");
                CloseClipboard();
                return;
            }

            wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
            if (!pMem) {
                GlobalFree(hMem);
                CloseClipboard();
                return;
            }

            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pMem, wideLength);
            GlobalUnlock(hMem);

            if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                GlobalFree(hMem);
                logger::warn("Failed to set clipboard data");
            }
        } catch (const std::exception& e) {
            logger::error("Exception while setting clipboard text: {}", e.what());
            if (hMem) {
                GlobalFree(hMem);
            }
        }

        CloseClipboard();
    }

    bool IsHighSurrogate(wchar_t ch) { return ch >= 0xD800 && ch <= 0xDBFF; }

    bool IsLowSurrogate(wchar_t ch) { return ch >= 0xDC00 && ch <= 0xDFFF; }

    bool ShouldQueueChar(wchar_t ch) { return ch >= 0x20 || ch == '\t'; }

    std::string ConvertUtf16ToUtf8(const wchar_t* text, int length) {
        if (!text || length <= 0) {
            return "";
        }

        int utf8Length = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0) {
            return "";
        }

        std::string result;
        try {
            result.resize(utf8Length);
            WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), utf8Length, nullptr, nullptr);
        } catch (const std::exception& e) {
            logger::error("Failed to allocate memory for committed text: {}", e.what());
            return "";
        }

        return result;
    }

    void InputHandler::QueueInputEvent(Cef::CefInputEvent event) {
        std::lock_guard lock(_eventQueueMutex);
        _eventQueue.emplace_back(std::move(event));
    }

    uint32_t InputHandler::GetMouseModifiers() const {
        uint32_t modifiers = WinKeyHandler::GetCefModifiers();
        if (_mouseButtonStates[0]) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
        if (_mouseButtonStates[1]) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
        if (_mouseButtonStates[2]) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
        return modifiers;
    }

    bool IsSystemKeyMessage(UINT uMsg) { return uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP || uMsg == WM_SYSCHAR; }

    void InputHandler::QueueCommittedCharEvent(const std::wstring& utf16Text, LPARAM lParam) {
        if (utf16Text.empty()) {
            return;
        }

        for (wchar_t ch : utf16Text) {
            QueueInputEvent(WinKeyHandler::CreateCharEvent(ch, lParam, false, _isFocusedTextInputActive.load()));
        }
    }

    std::wstring ConvertCodePointToUtf16(UINT codePoint) {
        if (codePoint > 0x10FFFF) {
            return L"";
        }

        if (codePoint <= 0xFFFF) {
            return std::wstring(1, static_cast<wchar_t>(codePoint));
        }

        codePoint -= 0x10000;
        wchar_t high = static_cast<wchar_t>(0xD800 + (codePoint >> 10));
        wchar_t low = static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF));

        return std::wstring{high, low};
    }

    LRESULT CALLBACK InputHandler::SubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR,
                                                DWORD_PTR dwRefData) {
        InputHandler& self = *reinterpret_cast<InputHandler*>(dwRefData);
        LRESULT imeControlResult = 0;
        if (self._imeHelper.HandleControlMessage(hwnd, uMsg, wParam, lParam, &imeControlResult)) {
            return imeControlResult;
        }

        if (uMsg == WM_IME_SETCONTEXT) {
            LPARAM imeLParam = lParam;
            self._imeHelper.ModifySetContextLParam(&imeLParam, uMsg);
            lParam = imeLParam;
        }

        if (self._isAnyInputCaptureActive.load()) {
            bool handledByUI = false;
            Core::PrismaViewId focusedViewIdCopy;
            {
                std::lock_guard lock(self._focusedViewIdMutex);
                focusedViewIdCopy = self._currentlyFocusedViewId;
            }

            if (focusedViewIdCopy != 0) {
                if (self._imeHelper.HandleMessage(hwnd, uMsg, wParam, lParam, focusedViewIdCopy, &handledByUI)) {
                    if (handledByUI) {
                        return 0;
                    }
                }

                switch (uMsg) {
                    case WM_KEYDOWN:
                    case WM_SYSKEYDOWN: {
                        self.QueueInputEvent(WinKeyHandler::CreateKeyEvent(Cef::CefInputKeyType::RawKeyDown, wParam,
                                                                           lParam, IsSystemKeyMessage(uMsg),
                                                                           self._isFocusedTextInputActive.load()));
                        handledByUI = true;
                        break;
                    }
                    case WM_KEYUP:
                    case WM_SYSKEYUP: {
                        self.QueueInputEvent(WinKeyHandler::CreateKeyEvent(Cef::CefInputKeyType::KeyUp, wParam, lParam,
                                                                           IsSystemKeyMessage(uMsg),
                                                                           self._isFocusedTextInputActive.load()));
                        handledByUI = true;
                        break;
                    }
                    case WM_CHAR:
                    case WM_SYSCHAR: {
                        handledByUI = true;
                        wchar_t ch = static_cast<wchar_t>(wParam);
                        if (IsHighSurrogate(ch)) {
                            self._pendingHighSurrogate = ch;
                            break;
                        }

                        std::wstring committedText;
                        if (IsLowSurrogate(ch) && self._pendingHighSurrogate != 0) {
                            committedText.push_back(self._pendingHighSurrogate);
                            committedText.push_back(ch);
                            self._pendingHighSurrogate = 0;
                        } else {
                            self._pendingHighSurrogate = 0;
                            if (ShouldQueueChar(ch)) {
                                committedText.push_back(ch);
                            }
                        }

                        self.QueueCommittedCharEvent(committedText, lParam);
                        break;
                    }
                    case WM_UNICHAR: {
                        if (wParam == UNICODE_NOCHAR) {
                            return TRUE;
                        }

                        handledByUI = true;
                        std::wstring committedText = ConvertCodePointToUtf16(static_cast<UINT>(wParam));
                        if (!committedText.empty() && ShouldQueueChar(committedText[0])) {
                            self._pendingHighSurrogate = 0;
                            self.QueueCommittedCharEvent(committedText, lParam);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            if (handledByUI) {
                return 0;
            }
        }

        switch (uMsg) {
            case WM_NCDESTROY:
            case WM_DESTROY:
            case WM_CLOSE:
                std::invoke(self._onExitCallback);
                break;
        }

        // Pass to next handler in the chain using DefSubclassProc
        // This properly handles the chain even if other mods are in the stack
        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    }

    InputHandler& InputHandler::GetSingleton() {
        static InputHandler singleton;
        return singleton;
    }

    RE::BSEventNotifyControl InputHandler::ProcessEvent(RE::InputEvent* const* a_event,
                                                        RE::BSTEventSource<RE::InputEvent*>*) {
        if (!a_event || !*a_event || !_isAnyInputCaptureActive.load()) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto cursor = RE::MenuCursor::GetSingleton();
        auto renderManager = RE::BSGraphics::Renderer::GetSingleton();
        if (!cursor || !renderManager) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto currentScreenSize = renderManager->GetScreenSize();
        const int cursorX = std::min(static_cast<int>(cursor->cursorPosX), static_cast<int>(currentScreenSize.width - 1));
        const int cursorY = std::min(static_cast<int>(cursor->cursorPosY), static_cast<int>(currentScreenSize.height - 1));

        for (auto event = *a_event; event; event = event->next) {
            switch (event->GetEventType()) {
                case RE::INPUT_EVENT_TYPE::kMouseMove: {
                    if (event->AsMouseMoveEvent()) {
                        Cef::CefInputMouseMove ev;
                        ev.x = cursorX;
                        ev.y = cursorY;
                        ev.modifiers = GetMouseModifiers();
                        ev.mouseLeave = false;
                        QueueInputEvent(ev);
                    }
                    break;
                }

                case RE::INPUT_EVENT_TYPE::kButton: {
                    auto buttonEvent = event->AsButtonEvent();
                    if (!buttonEvent || buttonEvent->GetDevice() != RE::INPUT_DEVICE::kMouse) break;

                    const auto idCode = buttonEvent->GetIDCode();
                    const bool isPressed = buttonEvent->IsPressed();
                    const bool isUp = buttonEvent->IsUp();

                    if (idCode <= 2) {
                        Cef::CefInputMouseButton button = Cef::CefInputMouseButton::Left;
                        switch (idCode) {
                            case 0:
                                button = Cef::CefInputMouseButton::Left;
                                break;
                            case 1:
                                button = Cef::CefInputMouseButton::Right;
                                break;
                            case 2:
                                button = Cef::CefInputMouseButton::Middle;
                                break;
                            default:
                                break;
                        }

                        if (isPressed && !_mouseButtonStates[idCode]) {
                            _mouseButtonStates[idCode] = true;
                            Cef::CefInputMouseClick ev;
                            ev.x = cursorX;
                            ev.y = cursorY;
                            ev.modifiers = GetMouseModifiers();
                            ev.button = button;
                            ev.mouseUp = false;
                            ev.clickCount = 1;
                            QueueInputEvent(ev);
                        } else if (isUp && _mouseButtonStates[idCode]) {
                            _mouseButtonStates[idCode] = false;
                            Cef::CefInputMouseClick ev;
                            ev.x = cursorX;
                            ev.y = cursorY;
                            ev.modifiers = GetMouseModifiers();
                            ev.button = button;
                            ev.mouseUp = true;
                            ev.clickCount = 1;
                            QueueInputEvent(ev);
                        }
                    } else if (idCode == 8 || idCode == 9) {
                        if (isPressed) {
                            int scrollPixelSize = 28;
                            Core::PrismaViewId focusedViewId;
                            {
                                std::lock_guard lock(_focusedViewIdMutex);
                                focusedViewId = _currentlyFocusedViewId;
                            }

                            if (focusedViewId != 0 && _viewsMap && _viewsMapMutex) {
                                std::shared_lock lock(*_viewsMapMutex);
                                auto it = _viewsMap->find(focusedViewId);
                                if (it != _viewsMap->end() && it->second) {
                                    scrollPixelSize = it->second->scrollingPixelSize;
                                }
                            }

                            const int scrollAmount = SCROLL_LINES_PER_WHEEL_DELTA * scrollPixelSize;
                            Cef::CefInputMouseWheel ev;
                            ev.x = cursorX;
                            ev.y = cursorY;
                            ev.modifiers = GetMouseModifiers();
                            ev.deltaX = 0;
                            ev.deltaY = idCode == 9 ? -scrollAmount : scrollAmount;
                            QueueInputEvent(ev);
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    bool InputHandler::Initialize(HWND gameHwnd,
                                  std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                                  std::shared_mutex* viewsMapMutex) {
        logger::info("Initialization...");

        if (auto inputEventSource = RE::BSInputDeviceManager::GetSingleton()) {
            inputEventSource->AddEventSink(this);
            logger::info("MouseEventListener registered with BSInputDeviceManager");
        } else {
            logger::error("Failed to register MouseEventListener: BSInputDeviceManager is null");
            return false;
        }

        _hWnd = gameHwnd;
        _viewsMap = viewsMap;
        _viewsMapMutex = viewsMapMutex;
        _isAnyInputCaptureActive = false;
        _isFocusedTextInputActive = false;
        {
            std::lock_guard lock(_focusedViewIdMutex);
            _currentlyFocusedViewId = 0;
        }

        _mouseButtonStates[0] = _mouseButtonStates[1] = _mouseButtonStates[2] = false;

        _imeHelper.SetCallbacks([](const std::string& s) { return EscapeForJS(s); },
                                [this](const std::wstring& ws, LPARAM lp) { QueueCommittedCharEvent(ws, lp); },
                                [](const wchar_t* p, int len) { return ConvertUtf16ToUtf8(p, len); });
        _imeHelper.SetContext({_hWnd, _viewsMap, _viewsMapMutex, &_focusedViewIdMutex, &_currentlyFocusedViewId,
                               &_isAnyInputCaptureActive, &_isFocusedTextInputActive});
        _imeHelper.Initialize(_hWnd);

        InstallWndProcHook();

        _isInitialized = true;

        logger::info("Initialized with HWND: {}", static_cast<void*>(_hWnd));

        return true;
    }

    void InputHandler::InstallWndProcHook() {
        // Thread-affinity issue with SetWindowSubclass:
        // - Windows requires SetWindowSubclass to be called from the window's owning thread
        // - The game HWND is created on the main thread (thread that creates the window)
        // - We are currently on the render thread (D3D Present hook)
        // - Behavior varies across systems:
        //   * Some systems: SetWindowSubclass works cross-thread (Windows 10+)
        //   * Other systems: SetWindowSubclass fails unless called from main thread
        // Solution: Try direct installation first (faster), fallback to main thread if it fails

        logger::info("Attempting to install WndProc hook from render thread...");
        if (InstallWndProcHookAttempt()) {
            logger::info("WndProc hook installed successfully from render thread.");
        } else {
            logger::warn("Direct installation failed, scheduling on main thread...");
            SKSE::GetTaskInterface()->AddTask([this] {
                logger::info("Attempting to install WndProc hook from main thread...");
                if (InstallWndProcHookAttempt()) {
                    logger::info("WndProc hook installed successfully from main thread.");
                } else {
                    logger::error("Failed to install WndProc hook even from main thread!");
                }
            });
        }
    }

    bool InputHandler::InstallWndProcHookAttempt() {
        logger::info("Attempting to install subclass on HWND: {:p}", static_cast<void*>(_hWnd));

        // Clear last error before calling
        SetLastError(0);

        if (!SetWindowSubclass(_hWnd, SubclassProc, SUBCLASS_ID, reinterpret_cast<DWORD_PTR>(this))) {
            DWORD err = GetLastError();
            logger::warn("Failed to install WndProc subclass. Error: {} (0x{:X})", err, err);
            return false;
        }

        logger::info("WndProc subclass installed successfully on HWND: {:p}", static_cast<void*>(_hWnd));
        return true;
    }

    void InputHandler::UninstallWndProcHook() const {
        RemoveWindowSubclass(_hWnd, SubclassProc, SUBCLASS_ID);
        logger::info("WndProc subclass removed");
    }

    void InputHandler::EnableInputCapture(Core::PrismaViewId viewId) {
        if (viewId == 0) {
            logger::warn("EnableInputCapture called with empty viewId.");
            return;
        }

        {
            std::lock_guard lock(_focusedViewIdMutex);
            if (_currentlyFocusedViewId != viewId) {
                _currentlyFocusedViewId = viewId;
                logger::debug("PrismaUI Input Capture focused on View [{}].", viewId);
            }
        }

        if (!_isAnyInputCaptureActive.exchange(true)) {
            logger::debug("PrismaUI Input Capture System Enabled for View [{}].", viewId);
        }

        _isFocusedTextInputActive.store(false);
        _imeHelper.SetAssociation(false);

        Communication::RegisterJSListener(viewId, IME_FOCUS_CALLBACK_NAME, [this, viewId](std::string focused) {
            Core::PrismaViewId currentFocusedViewId;
            {
                std::lock_guard lock(_focusedViewIdMutex);
                currentFocusedViewId = _currentlyFocusedViewId;
            }

            if (currentFocusedViewId != viewId) {
                return;
            }

            const bool isTextInputFocused = focused == "1";
            const bool isCaptureActive = _isAnyInputCaptureActive.load();
            _isFocusedTextInputActive.store(isTextInputFocused);
            _imeHelper.SetAssociation(isCaptureActive && isTextInputFocused);
        });
        RefreshImeFocusTrackingForView(viewId);

        _mouseButtonStates[0] = _mouseButtonStates[1] = _mouseButtonStates[2] = false;
    }

    void InputHandler::DisableInputCapture(Core::PrismaViewId viewIdToUnfocus) {
        bool disableSystem = false;
        Core::PrismaViewId currentFocusedBeforeDisable;
        {
            std::lock_guard lock(_focusedViewIdMutex);
            currentFocusedBeforeDisable = _currentlyFocusedViewId;
            if (viewIdToUnfocus == 0 || viewIdToUnfocus == _currentlyFocusedViewId) {
                if (_isAnyInputCaptureActive.load()) {
                    disableSystem = true;
                    _currentlyFocusedViewId = 0;
                }
            }
        }

        if (disableSystem) {
            if (_isAnyInputCaptureActive.exchange(false)) {
                _isFocusedTextInputActive.store(false);
                _imeHelper.SetAssociation(false);
                logger::debug("PrismaUI Input Capture System Disabled (was active for View [{}]).",
                              currentFocusedBeforeDisable);

                _mouseButtonStates[0] = _mouseButtonStates[1] = _mouseButtonStates[2] = false;

                if (currentFocusedBeforeDisable != 0) {
                    Cef::CefInputMouseMove resetEvent;
                    resetEvent.x = 0;
                    resetEvent.y = 0;
                    resetEvent.modifiers = GetMouseModifiers();
                    resetEvent.mouseLeave = false;
                    std::vector<Cef::CefInputEvent> resetEvents;
                    resetEvents.emplace_back(resetEvent);
                    Cef::CefRuntime::GetSingleton().DispatchInputEvents(currentFocusedBeforeDisable,
                                                                        std::move(resetEvents));
                }
            }
        } else if (viewIdToUnfocus != 0) {
            logger::debug(
                "PrismaUI: DisableInputCapture called for View [{}] but View [{}] is/was focused. No change to system "
                "state, only unfocused ID removed if it matched.",
                viewIdToUnfocus, currentFocusedBeforeDisable);
        }
    }

    void InputHandler::ClearImeState(Core::PrismaViewId viewId) {
        if (viewId == 0) {
            return;
        }

        _isFocusedTextInputActive.store(false);
        _imeHelper.SetAssociation(false);
    }

    bool InputHandler::IsAnyInputCaptureActive() const { return _isAnyInputCaptureActive.load(); }

    void InputHandler::ProcessEvents() {
        Core::PrismaViewId focusedViewIdCopy;
        {
            std::lock_guard lock(_focusedViewIdMutex);
            focusedViewIdCopy = _currentlyFocusedViewId;
        }

        if (focusedViewIdCopy == 0) {
            std::lock_guard lock(_eventQueueMutex);
            if (!_eventQueue.empty()) {
                logger::debug("Dropping {} queued input event(s): no focused PrismaUI view.", _eventQueue.size());
                _eventQueue.clear();
            }
            return;
        }

        std::vector<Cef::CefInputEvent> eventsToProcess;
        {
            std::lock_guard lock(_eventQueueMutex);
            if (_eventQueue.empty()) return;
            eventsToProcess.swap(_eventQueue);
        }

        std::shared_ptr<Core::PrismaView> targetViewData = nullptr;
        if (_viewsMap && _viewsMapMutex) {
            std::shared_lock lock(*_viewsMapMutex);
            auto it = _viewsMap->find(focusedViewIdCopy);
            if (it != _viewsMap->end()) {
                targetViewData = it->second;
            }
        }

        if (!targetViewData) {
            logger::warn("Dropping {} queued input event(s): focused View [{}] is missing.", eventsToProcess.size(),
                         focusedViewIdCopy);
            return;
        }

        if (targetViewData->isHidden.load()) {
            logger::debug("Dropping {} queued input event(s): focused View [{}] is hidden.", eventsToProcess.size(),
                          focusedViewIdCopy);
            return;
        }

        if (!targetViewData->isFocused.load()) {
            logger::debug("Dropping {} queued input event(s): View [{}] no longer owns native focus.",
                          eventsToProcess.size(), focusedViewIdCopy);
            return;
        }

        Cef::CefRuntime::GetSingleton().DispatchInputEvents(focusedViewIdCopy, std::move(eventsToProcess));
    }

    void InputHandler::Shutdown() {
        if (!_isInitialized) {
            return;
        }

        logger::info("Shutdown...");

        DisableInputCapture(0);
        {
            std::lock_guard lock(_eventQueueMutex);
            _eventQueue.clear();
        }

        auto inputEventSource = RE::BSInputDeviceManager::GetSingleton();
        if (inputEventSource) {
            inputEventSource->RemoveEventSink(this);
            logger::debug("MouseEventListener removed from BSInputDeviceManager");
        }

        UninstallWndProcHook();

        _imeHelper.Shutdown(_hWnd);

        _hWnd = nullptr;
        _viewsMap = nullptr;
        _viewsMapMutex = nullptr;
        _isFocusedTextInputActive = false;
        _isInitialized = false;
        logger::info("Shutdown complete");
    }

    void InputHandler::OnExit(std::move_only_function<void()>&& callback) { _onExitCallback = std::move(callback); }
}
