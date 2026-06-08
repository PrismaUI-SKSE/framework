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

namespace PrismaUI::InputHandler {
    using namespace Core;

    HWND g_hWnd = nullptr;
    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* g_viewsMap = nullptr;
    std::shared_mutex* g_viewsMapMutex = nullptr;

    Core::PrismaViewId g_currentlyFocusedViewId;
    std::mutex g_focusedViewIdMutex;

    std::atomic<bool> g_isAnyInputCaptureActive = false;

    std::mutex g_eventQueueMutex;
    std::vector<Cef::CefInputEvent> g_eventQueue;

    const int SCROLL_LINES_PER_WHEEL_DELTA = 1;

    // Clipboard safety limits
    constexpr size_t MAX_CLIPBOARD_SIZE = 1024 * 1024;  // 1MB max
    constexpr size_t MAX_CLIPBOARD_CHARS = 200000;      // 200K characters max

    bool g_mouseButtonStates[3] = {false, false, false};
    wchar_t g_pendingHighSurrogate = 0;

    // WndProc subclass state
    static std::atomic<bool> g_wndProcInstalled = false;
    static constexpr UINT_PTR SUBCLASS_ID = 0x505249534D41;  // "PRISMA" in hex
    static std::mutex g_wndProcMutex;                        // Thread-safe installation

    static ImeHelper g_imeHelper;
    static std::atomic<bool> g_isFocusedTextInputActive = false;

    constexpr const char* IME_FOCUS_CALLBACK_NAME = Cef::Messages::kImeFocusListener;

    void RefreshImeFocusTrackingForView(const Core::PrismaViewId& viewId) {
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

    std::string GetClipboardText() {
        if (!OpenClipboard(g_hWnd)) {
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

    void SetClipboardText(const std::string& text) {
        // Check size limits before processing
        if (text.size() > MAX_CLIPBOARD_SIZE) {
            logger::warn("Text too large to copy to clipboard: {} bytes (max: {} bytes)", text.size(),
                         MAX_CLIPBOARD_SIZE);
            return;
        }

        if (!OpenClipboard(g_hWnd)) {
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

    void QueueInputEvent(Cef::CefInputEvent event) {
        std::lock_guard lock(g_eventQueueMutex);
        g_eventQueue.emplace_back(std::move(event));
    }

    uint32_t GetMouseModifiers() {
        uint32_t modifiers = WinKeyHandler::GetCefModifiers();
        if (g_mouseButtonStates[0]) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
        if (g_mouseButtonStates[1]) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
        if (g_mouseButtonStates[2]) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
        return modifiers;
    }

    bool IsSystemKeyMessage(UINT uMsg) { return uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP || uMsg == WM_SYSCHAR; }

    void QueueCommittedCharEvent(const std::wstring& utf16Text, LPARAM lParam) {
        if (utf16Text.empty()) {
            return;
        }

        for (wchar_t ch : utf16Text) {
            QueueInputEvent(WinKeyHandler::CreateCharEvent(ch, lParam, false, g_isFocusedTextInputActive.load()));
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

    class MouseEventListener : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static MouseEventListener* GetSingleton() {
            static MouseEventListener singleton;
            return &singleton;
        }

        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* a_event,
            [[maybe_unused]] RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override {
            if (!a_event || !*a_event || !g_isAnyInputCaptureActive.load()) {
                return RE::BSEventNotifyControl::kContinue;
            }

            auto cursor = RE::MenuCursor::GetSingleton();
            if (!cursor) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const int cursorX = static_cast<int>(cursor->cursorPosX);
            const int cursorY = static_cast<int>(cursor->cursorPosY);

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

                            if (isPressed && !g_mouseButtonStates[idCode]) {
                                g_mouseButtonStates[idCode] = true;
                                Cef::CefInputMouseClick ev;
                                ev.x = cursorX;
                                ev.y = cursorY;
                                ev.modifiers = GetMouseModifiers();
                                ev.button = button;
                                ev.mouseUp = false;
                                ev.clickCount = 1;
                                QueueInputEvent(ev);
                            } else if (isUp && g_mouseButtonStates[idCode]) {
                                g_mouseButtonStates[idCode] = false;
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
                                    std::lock_guard lock(g_focusedViewIdMutex);
                                    focusedViewId = g_currentlyFocusedViewId;
                                }

                                if (focusedViewId != 0 && g_viewsMap && g_viewsMapMutex) {
                                    std::shared_lock lock(*g_viewsMapMutex);
                                    auto it = g_viewsMap->find(focusedViewId);
                                    if (it != g_viewsMap->end() && it->second) {
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
    };

    LRESULT CALLBACK SubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                                  DWORD_PTR /*dwRefData*/) {
        LRESULT imeControlResult = 0;
        if (g_imeHelper.HandleControlMessage(hwnd, uMsg, wParam, lParam, &imeControlResult)) {
            return imeControlResult;
        }

        if (uMsg == WM_IME_SETCONTEXT) {
            LPARAM imeLParam = lParam;
            g_imeHelper.ModifySetContextLParam(&imeLParam, uMsg);
            lParam = imeLParam;
        }

        if (g_isAnyInputCaptureActive.load()) {
            bool handledByUI = false;
            Core::PrismaViewId focusedViewIdCopy;
            {
                std::lock_guard lock(g_focusedViewIdMutex);
                focusedViewIdCopy = g_currentlyFocusedViewId;
            }

            if (focusedViewIdCopy != 0) {
                if (g_imeHelper.HandleMessage(hwnd, uMsg, wParam, lParam, focusedViewIdCopy, &handledByUI)) {
                    if (handledByUI) {
                        return 0;
                    }
                }

                switch (uMsg) {
                    case WM_KEYDOWN:
                    case WM_SYSKEYDOWN: {
                        QueueInputEvent(WinKeyHandler::CreateKeyEvent(Cef::CefInputKeyType::RawKeyDown, wParam, lParam,
                                                                      IsSystemKeyMessage(uMsg),
                                                                      g_isFocusedTextInputActive.load()));
                        handledByUI = true;
                        break;
                    }
                    case WM_KEYUP:
                    case WM_SYSKEYUP: {
                        QueueInputEvent(WinKeyHandler::CreateKeyEvent(Cef::CefInputKeyType::KeyUp, wParam, lParam,
                                                                      IsSystemKeyMessage(uMsg),
                                                                      g_isFocusedTextInputActive.load()));
                        handledByUI = true;
                        break;
                    }
                    case WM_CHAR:
                    case WM_SYSCHAR: {
                        handledByUI = true;
                        wchar_t ch = static_cast<wchar_t>(wParam);
                        if (IsHighSurrogate(ch)) {
                            g_pendingHighSurrogate = ch;
                            break;
                        }

                        std::wstring committedText;
                        if (IsLowSurrogate(ch) && g_pendingHighSurrogate != 0) {
                            committedText.push_back(g_pendingHighSurrogate);
                            committedText.push_back(ch);
                            g_pendingHighSurrogate = 0;
                        } else {
                            g_pendingHighSurrogate = 0;
                            if (ShouldQueueChar(ch)) {
                                committedText.push_back(ch);
                            }
                        }

                        QueueCommittedCharEvent(committedText, lParam);
                        break;
                    }
                    case WM_UNICHAR: {
                        if (wParam == UNICODE_NOCHAR) {
                            return TRUE;
                        }

                        handledByUI = true;
                        std::wstring committedText = ConvertCodePointToUtf16(static_cast<UINT>(wParam));
                        if (!committedText.empty() && ShouldQueueChar(committedText[0])) {
                            g_pendingHighSurrogate = 0;
                            QueueCommittedCharEvent(committedText, lParam);
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

        // Handle window destruction - clean up subclass
        if (uMsg == WM_NCDESTROY) {
            RemoveWindowSubclass(hwnd, SubclassProc, uIdSubclass);
            if (uIdSubclass == SUBCLASS_ID) {
                g_wndProcInstalled.store(false);
            }
        }

        // Pass to next handler in the chain using DefSubclassProc
        // This properly handles the chain even if other mods are in the stack
        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    }

    void Initialize(HWND gameHwnd, std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                    std::shared_mutex* viewsMapMutex) {
        g_hWnd = gameHwnd;
        g_viewsMap = viewsMap;
        g_viewsMapMutex = viewsMapMutex;
        g_isAnyInputCaptureActive = false;
        g_isFocusedTextInputActive = false;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            g_currentlyFocusedViewId = 0;
        }

        g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;

        logger::info("PrismaUI::InputHandler Initialized with HWND: {}", (void*)g_hWnd);

        g_imeHelper.SetCallbacks([](const std::string& s) { return EscapeForJS(s); },
                                 [](const std::wstring& ws, LPARAM lp) { QueueCommittedCharEvent(ws, lp); },
                                 [](const wchar_t* p, int len) { return ConvertUtf16ToUtf8(p, len); });
        g_imeHelper.SetContext({g_hWnd, g_viewsMap, g_viewsMapMutex, &g_focusedViewIdMutex, &g_currentlyFocusedViewId,
                                &g_isAnyInputCaptureActive, &g_isFocusedTextInputActive});
        g_imeHelper.Initialize(g_hWnd);

        auto inputEventSource = RE::BSInputDeviceManager::GetSingleton();
        if (inputEventSource) {
            inputEventSource->AddEventSink(MouseEventListener::GetSingleton());
            logger::info("MouseEventListener registered with BSInputDeviceManager");
        } else {
            logger::error("Failed to register MouseEventListener: BSInputDeviceManager is null");
        }
    }

    bool InstallWndProcHook() {
        std::lock_guard<std::mutex> lock(g_wndProcMutex);

        if (g_wndProcInstalled.load()) {
            logger::debug("WndProc subclass already installed");
            return true;
        }

        if (!g_hWnd) {
            logger::error("Cannot install WndProc subclass: HWND is null");
            return false;
        }

        // Check if HWND is valid
        if (!IsWindow(g_hWnd)) {
            logger::error("HWND {:p} is not a valid window", (void*)g_hWnd);
            return false;
        }

        logger::debug("Attempting to install subclass on HWND: {:p}", (void*)g_hWnd);

        // Clear last error before calling
        SetLastError(0);

        if (!SetWindowSubclass(g_hWnd, SubclassProc, SUBCLASS_ID, 0)) {
            DWORD err = GetLastError();
            logger::warn("Failed to install WndProc subclass. Error: {} (0x{:X})", err, err);
            return false;
        }

        g_wndProcInstalled.store(true);
        logger::info("WndProc subclass installed successfully on HWND: {:p}", (void*)g_hWnd);
        return true;
    }

    void UninstallWndProcHook() {
        std::lock_guard<std::mutex> lock(g_wndProcMutex);

        if (!g_wndProcInstalled.exchange(false)) {
            return;
        }

        if (g_hWnd) {
            RemoveWindowSubclass(g_hWnd, SubclassProc, SUBCLASS_ID);
            logger::info("WndProc subclass removed");
        }
    }

    void EnableInputCapture(const Core::PrismaViewId& viewId) {
        if (viewId == 0) {
            logger::warn("EnableInputCapture called with empty viewId.");
            return;
        }
        bool firstTimeActivation = false;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            if (g_currentlyFocusedViewId != viewId) {
                g_currentlyFocusedViewId = viewId;
                logger::debug("PrismaUI Input Capture focused on View [{}].", viewId);
            }
        }

        if (!g_isAnyInputCaptureActive.exchange(true)) {
            firstTimeActivation = true;
            logger::debug("PrismaUI Input Capture System Enabled for View [{}].", viewId);
        }

        g_isFocusedTextInputActive.store(false);
        g_imeHelper.SetAssociation(false);

        Communication::RegisterJSListener(viewId, IME_FOCUS_CALLBACK_NAME, [viewId](std::string focused) {
            Core::PrismaViewId currentFocusedViewId = 0;
            {
                std::lock_guard lock(g_focusedViewIdMutex);
                currentFocusedViewId = g_currentlyFocusedViewId;
            }

            if (currentFocusedViewId != viewId) {
                return;
            }

            const bool isTextInputFocused = focused == "1";
            const bool isCaptureActive = g_isAnyInputCaptureActive.load();
            g_isFocusedTextInputActive.store(isTextInputFocused);
            g_imeHelper.SetAssociation(isCaptureActive && isTextInputFocused);

            if (!isTextInputFocused) {
                logger::debug("IME text focus cleared for View [{}].", viewId);
                g_imeHelper.ClearStateInJS(viewId);
            } else {
                logger::debug("IME text focus active for View [{}].", viewId);
            }
        });
        RefreshImeFocusTrackingForView(viewId);

        g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;
    }

    void DisableInputCapture(const Core::PrismaViewId& viewIdToUnfocus) {
        bool disableSystem = false;
        Core::PrismaViewId currentFocusedBeforeDisable;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            currentFocusedBeforeDisable = g_currentlyFocusedViewId;
            if (viewIdToUnfocus == 0 || viewIdToUnfocus == g_currentlyFocusedViewId) {
                if (g_isAnyInputCaptureActive.load()) {
                    disableSystem = true;
                    g_currentlyFocusedViewId = 0;
                }
            }
        }

        if (disableSystem) {
            if (g_isAnyInputCaptureActive.exchange(false)) {
                g_isFocusedTextInputActive.store(false);
                g_imeHelper.SetAssociation(false);
                logger::debug("PrismaUI Input Capture System Disabled (was active for View [{}]).",
                              currentFocusedBeforeDisable);

                g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;

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

    void ClearImeState(const Core::PrismaViewId& viewId) {
        if (viewId == 0) {
            return;
        }

        g_isFocusedTextInputActive.store(false);
        g_imeHelper.SetAssociation(false);
        g_imeHelper.ClearStateInJS(viewId);
    }

    bool IsAnyInputCaptureActive() { return g_isAnyInputCaptureActive.load(); }

    bool IsInputCaptureActiveForView(const Core::PrismaViewId& viewId) {
        Core::PrismaViewId currentFocused;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            currentFocused = g_currentlyFocusedViewId;
        }
        if (viewId == 0) return g_isAnyInputCaptureActive.load();
        return g_isAnyInputCaptureActive.load() && (currentFocused == viewId);
    }

    void ProcessEvents() {
        Core::PrismaViewId focusedViewIdCopy;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            focusedViewIdCopy = g_currentlyFocusedViewId;
        }

        if (focusedViewIdCopy == 0) {
            std::lock_guard lock(g_eventQueueMutex);
            if (!g_eventQueue.empty()) {
                logger::debug("Dropping {} queued input event(s): no focused PrismaUI view.", g_eventQueue.size());
                g_eventQueue.clear();
            }
            return;
        }

        std::vector<Cef::CefInputEvent> eventsToProcess;
        {
            std::lock_guard lock(g_eventQueueMutex);
            if (g_eventQueue.empty()) return;
            eventsToProcess.swap(g_eventQueue);
        }

        std::shared_ptr<Core::PrismaView> targetViewData = nullptr;
        if (g_viewsMap && g_viewsMapMutex) {
            std::shared_lock lock(*g_viewsMapMutex);
            auto it = g_viewsMap->find(focusedViewIdCopy);
            if (it != g_viewsMap->end()) {
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

    void Shutdown() {
        DisableInputCapture(0);
        {
            std::lock_guard lock(g_eventQueueMutex);
            g_eventQueue.clear();
        }

        auto inputEventSource = RE::BSInputDeviceManager::GetSingleton();
        if (inputEventSource) {
            inputEventSource->RemoveEventSink(MouseEventListener::GetSingleton());
            logger::debug("MouseEventListener removed from BSInputDeviceManager");
        }

        UninstallWndProcHook();

        g_imeHelper.Shutdown(g_hWnd);

        g_hWnd = nullptr;
        g_viewsMap = nullptr;
        g_viewsMapMutex = nullptr;
        g_isFocusedTextInputActive = false;
        logger::info("PrismaUI::InputHandler Shutdown.");
    }
}
