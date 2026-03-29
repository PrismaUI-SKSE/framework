#include "InputHandler.h"

#include <algorithm>
#include <commctrl.h>

#include "Communication.h"
#include "Core.h"
#include "ImeHelper.h"
#include "Utils/Encoding.h"
#pragma comment(lib, "comctl32.lib")

namespace PrismaUI::InputHandler {
    using namespace Core;

    HWND g_hWnd = nullptr;
    SingleThreadExecutor* g_ultralightThreadExecutor = nullptr;
    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* g_viewsMap = nullptr;
    std::shared_mutex* g_viewsMapMutex = nullptr;

    Core::PrismaViewId g_currentlyFocusedViewId;
    std::mutex g_focusedViewIdMutex;

    std::atomic<bool> g_isAnyInputCaptureActive = false;

    std::mutex g_eventQueueMutex;
    std::vector<InputEvent> g_eventQueue;

    const int SCROLL_LINES_PER_WHEEL_DELTA = 1;

    // Clipboard safety limits
    constexpr size_t MAX_CLIPBOARD_SIZE = 1024 * 1024;  // 1MB max
    constexpr size_t MAX_CLIPBOARD_CHARS = 200000;      // 200K characters max

    bool g_mouseButtonStates[3] = {false, false, false};
    wchar_t g_pendingHighSurrogate = 0;

    // Native cursor: drives Ultralight and RE::MenuCursor while capture is active
    static std::atomic<float> g_nativeCursorX{0.0f};
    static std::atomic<float> g_nativeCursorY{0.0f};

    // WndProc subclass state
    static std::atomic<bool> g_wndProcInstalled = false;
    static constexpr UINT_PTR SUBCLASS_ID = 0x505249534D41;  // "PRISMA" in hex
    static std::mutex g_wndProcMutex;                        // Thread-safe installation

    static ImeHelper g_imeHelper;
    static std::atomic<bool> g_isFocusedTextInputActive = false;

    constexpr const char* IME_FOCUS_CALLBACK_NAME = "__prismaNativeImeFocusChanged";

    std::string BuildImeFocusTrackingScript() {
        const std::string callbackName = IME_FOCUS_CALLBACK_NAME;
        return "(function(){"
               "if(window.__prismaImeFocusTrackingInstalled){"
               "if(typeof window.__prismaImeFocusNotify==='function'){window.__prismaImeFocusNotify(document.activeElement);}"
               "return;"
               "}"
               "function isTextInputElement(el){"
               "if(!el||el.disabled||el.readOnly)return false;"
               "if(el.isContentEditable)return true;"
               "var tag=(el.tagName||'').toUpperCase();"
               "if(tag==='TEXTAREA')return true;"
               "if(tag!=='INPUT')return false;"
               "var type=((el.type||'text')+'').toLowerCase();"
               "switch(type){"
               "case '':case 'text':case 'search':case 'url':case 'tel':case 'password':case 'email':case 'number':"
               "return true;"
               "default:return false;"
               "}"
               "}"
               "function notify(element){"
               "var focused=isTextInputElement(element)?'1':'0';"
               "if(typeof window['" + callbackName + "']==='function'){window['" + callbackName + "'](focused);}"
               "}"
               "window.__prismaImeFocusNotify=notify;"
               "window.__prismaImeFocusTrackingInstalled=true;"
               "document.addEventListener('focusin',function(event){notify(event.target);},true);"
               "document.addEventListener('focusout',function(){setTimeout(function(){notify(document.activeElement);},0);},true);"
               "notify(document.activeElement);"
               "})();";
    }

    void InstallImeFocusTrackingForView(const Core::PrismaViewId& viewId) {
        if (!g_ultralightThreadExecutor || !g_viewsMap || !g_viewsMapMutex || viewId == 0) {
            return;
        }

        auto install = [viewId]() {
            std::shared_ptr<Core::PrismaView> viewData = nullptr;
            {
                std::shared_lock lock(*g_viewsMapMutex);
                auto it = g_viewsMap->find(viewId);
                if (it != g_viewsMap->end()) {
                    viewData = it->second;
                }
            }

            if (!viewData || !viewData->ultralightView || !viewData->isLoadingFinished.load()) {
                return;
            }

            Communication::BindJSCallbacks(viewId);

            try {
                ultralight::String script = BuildImeFocusTrackingScript().c_str();
                viewData->ultralightView->EvaluateScript(script, nullptr, "");
            } catch (const std::exception& e) {
                logger::error("Failed to install IME focus tracking for View [{}]: {}", viewId, e.what());
            } catch (...) {
                logger::error("Failed to install IME focus tracking for View [{}]: unknown exception", viewId);
            }
        };

        if (g_ultralightThreadExecutor->IsWorkerThread()) {
            install();
            return;
        }

        g_ultralightThreadExecutor->submit(install);
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

    void QueueCommittedCharEvent(const std::wstring& utf16Text, LPARAM lParam) {
        if (utf16Text.empty()) {
            return;
        }

        std::string utf8Text = ConvertUtf16ToUtf8(utf16Text.data(), static_cast<int>(utf16Text.size()));
        if (utf8Text.empty()) {
            logger::warn("Failed to convert committed text to UTF-8");
            return;
        }

        ultralight::KeyEvent charEvent;
        charEvent.type = ultralight::KeyEvent::kType_Char;
        WinKeyHandler::GetUltralightModifiers(charEvent);

        ultralight::String ulText = ultralight::Convert(utf8Text);
        charEvent.text = ulText;
        charEvent.unmodified_text = ulText;
        charEvent.virtual_key_code = ultralight::KeyCodes::GK_UNKNOWN;
        charEvent.native_key_code = 0;
        charEvent.key_identifier = "";
        charEvent.is_keypad = false;
        charEvent.is_auto_repeat = (HIWORD(lParam) & KF_REPEAT) == KF_REPEAT;
        charEvent.is_system_key = false;

        std::lock_guard lock(g_eventQueueMutex);
        g_eventQueue.emplace_back(charEvent);
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

    static void SetNativeCursorPosition(float x, float y) {
        float screenW = static_cast<float>(Core::screenSize.width);
        float screenH = static_cast<float>(Core::screenSize.height);
        x = std::clamp(x, 0.0f, screenW > 0 ? screenW - 1.0f : 0.0f);
        y = std::clamp(y, 0.0f, screenH > 0 ? screenH - 1.0f : 0.0f);
        g_nativeCursorX.store(x, std::memory_order_relaxed);
        g_nativeCursorY.store(y, std::memory_order_relaxed);

        if (auto* menuCursor = RE::MenuCursor::GetSingleton()) {
            menuCursor->cursorPosX = x;
            menuCursor->cursorPosY = y;
        }
    }

    // Align native + Ultralight coordinates to the game menu cursor when capture starts
    static void SyncNativeCursorFromMenuCursor() {
        auto* menuCursor = RE::MenuCursor::GetSingleton();
        if (!menuCursor) {
            return;
        }

        SetNativeCursorPosition(menuCursor->cursorPosX, menuCursor->cursorPosY);

        ultralight::MouseEvent ev;
        ev.type = ultralight::MouseEvent::kType_MouseMoved;
        ev.x = static_cast<int>(g_nativeCursorX.load(std::memory_order_relaxed));
        ev.y = static_cast<int>(g_nativeCursorY.load(std::memory_order_relaxed));
        ev.button = ultralight::MouseEvent::kButton_None;

        std::lock_guard lock(g_eventQueueMutex);
        g_eventQueue.emplace_back(ev);
    }

    static void AccumulateCursorDelta(float deltaX, float deltaY) {
        float curX = g_nativeCursorX.load(std::memory_order_relaxed) + deltaX;
        float curY = g_nativeCursorY.load(std::memory_order_relaxed) + deltaY;
        SetNativeCursorPosition(curX, curY);
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

            for (auto event = *a_event; event; event = event->next) {
                switch (event->GetEventType()) {
                    case RE::INPUT_EVENT_TYPE::kMouseMove: {
                        auto mouseMoveEvent = event->AsMouseMoveEvent();
                        if (mouseMoveEvent) {
                            AccumulateCursorDelta(
                                static_cast<float>(mouseMoveEvent->mouseInputX),
                                static_cast<float>(mouseMoveEvent->mouseInputY));

                            float curX = g_nativeCursorX.load(std::memory_order_relaxed);
                            float curY = g_nativeCursorY.load(std::memory_order_relaxed);

                            ultralight::MouseEvent ev;
                            ev.type = ultralight::MouseEvent::kType_MouseMoved;
                            ev.x = static_cast<int>(curX);
                            ev.y = static_cast<int>(curY);
                            ev.button = ultralight::MouseEvent::kButton_None;

                            std::lock_guard lock(g_eventQueueMutex);
                            g_eventQueue.emplace_back(ev);
                        }
                        break;
                    }

                    case RE::INPUT_EVENT_TYPE::kButton: {
                        auto buttonEvent = event->AsButtonEvent();
                        if (!buttonEvent || buttonEvent->GetDevice() != RE::INPUT_DEVICE::kMouse) break;

                        const auto idCode = buttonEvent->GetIDCode();
                        bool isPressed = buttonEvent->IsPressed();
                        bool isUp = buttonEvent->IsUp();

                        int cursorX = static_cast<int>(g_nativeCursorX.load(std::memory_order_relaxed));
                        int cursorY = static_cast<int>(g_nativeCursorY.load(std::memory_order_relaxed));

                        if (idCode <= 2) {
                            ultralight::MouseEvent::Button button = ultralight::MouseEvent::kButton_None;
                            switch (idCode) {
                                case 0:
                                    button = ultralight::MouseEvent::kButton_Left;
                                    break;
                                case 1:
                                    button = ultralight::MouseEvent::kButton_Right;
                                    break;
                                case 2:
                                    button = ultralight::MouseEvent::kButton_Middle;
                                    break;
                            }

                            if (isPressed && !g_mouseButtonStates[idCode]) {
                                g_mouseButtonStates[idCode] = true;

                                ultralight::MouseEvent ev;
                                ev.type = ultralight::MouseEvent::kType_MouseDown;
                                ev.x = cursorX;
                                ev.y = cursorY;
                                ev.button = button;

                                std::lock_guard lock(g_eventQueueMutex);
                                g_eventQueue.emplace_back(ev);
                            } else if (isUp && g_mouseButtonStates[idCode]) {
                                g_mouseButtonStates[idCode] = false;

                                ultralight::MouseEvent ev;
                                ev.type = ultralight::MouseEvent::kType_MouseUp;
                                ev.x = cursorX;
                                ev.y = cursorY;
                                ev.button = button;

                                std::lock_guard lock(g_eventQueueMutex);
                                g_eventQueue.emplace_back(ev);
                            }
                        }

                        else if (idCode == 8 || idCode == 9) {
                            if (isPressed) {
                                ScrollEventWithPosition scrollWithPos;
                                scrollWithPos.event.type = ultralight::ScrollEvent::kType_ScrollByPixel;
                                scrollWithPos.event.delta_x = 0;
                                scrollWithPos.mouseX = cursorX;
                                scrollWithPos.mouseY = cursorY;

                                int scrollPixelSize = 28;

                                Core::PrismaViewId focusedViewId;
                                {
                                    std::lock_guard lock(g_focusedViewIdMutex);
                                    focusedViewId = g_currentlyFocusedViewId;
                                }

                                if (focusedViewId != 0) {
                                    std::shared_lock lock(*g_viewsMapMutex);
                                    auto it = g_viewsMap->find(focusedViewId);
                                    if (it != g_viewsMap->end() && it->second) {
                                        scrollPixelSize = it->second->scrollingPixelSize;
                                    }
                                }

                                int scrollAmount = SCROLL_LINES_PER_WHEEL_DELTA * scrollPixelSize;
                                if (idCode == 9) {
                                    scrollWithPos.event.delta_y = -scrollAmount;
                                } else {
                                    scrollWithPos.event.delta_y = scrollAmount;
                                }

                                std::lock_guard lock(g_eventQueueMutex);
                                g_eventQueue.emplace_back(scrollWithPos);
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
                    case WM_KEYDOWN: {
                        // Handle Ctrl+V (Paste)
                        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'V') {
                            const bool viewHasInputFieldFocus = g_isFocusedTextInputActive.load();
                            if (viewHasInputFieldFocus) {
                                try {
                                    std::string clipboardText = GetClipboardText();
                                    if (!clipboardText.empty()) {
                                        logger::debug("Ctrl+V: Pasting {} characters from clipboard",
                                                      clipboardText.length());

                                        // Escape text for JavaScript string
                                        std::string escapedText = EscapeForJS(clipboardText);

                                        if (escapedText.empty() && !clipboardText.empty()) {
                                            logger::warn("Failed to escape clipboard text, paste cancelled");
                                        } else if (!escapedText.empty()) {
                                            // Use JavaScript execCommand to insert text properly (handles UTF-8)
                                            std::string script =
                                                "document.execCommand('insertText', false, '" + escapedText + "')";

                                            PrismaUI::Communication::Invoke(focusedViewIdCopy, script.c_str());
                                        }
                                    }
                                } catch (const std::exception& e) {
                                    logger::error("Exception during paste operation: {}", e.what());
                                }
                            }
                            handledByUI = true;
                            break;
                        }

                        // Handle Ctrl+C (Copy)
                        if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C') {
                            // Execute JavaScript to get selected text on ultralightThread
                            if (g_ultralightThreadExecutor && g_viewsMap && g_viewsMapMutex) {
                                g_ultralightThreadExecutor->submit([viewId = focusedViewIdCopy]() {
                                    try {
                                        std::shared_ptr<Core::PrismaView> viewData = nullptr;
                                        {
                                            std::shared_lock lock(*g_viewsMapMutex);
                                            auto it = g_viewsMap->find(viewId);
                                            if (it != g_viewsMap->end()) {
                                                viewData = it->second;
                                            }
                                        }

                                        if (viewData && viewData->ultralightView) {
                                            ultralight::String script = "window.getSelection().toString()";
                                            ultralight::String result =
                                                viewData->ultralightView->EvaluateScript(script, nullptr, "");
                                            std::string selectedText = result.utf8().data();

                                            // Check size before copying
                                            if (selectedText.size() > MAX_CLIPBOARD_SIZE) {
                                                logger::warn(
                                                    "Selected text too large to copy: {} bytes (max: {} bytes)",
                                                    selectedText.size(), MAX_CLIPBOARD_SIZE);
                                                return;
                                            }

                                            if (!selectedText.empty()) {
                                                // Copy to clipboard on Skyrim main thread to avoid blocking
                                                SKSE::GetTaskInterface()->AddTask([text = selectedText]() {
                                                    SetClipboardText(text);
                                                    logger::debug("Ctrl+C: Copied {} characters to clipboard",
                                                                  text.length());
                                                });
                                            }
                                        }
                                    } catch (const std::exception& e) {
                                        logger::error("Exception during copy operation: {}", e.what());
                                    }
                                });
                            }
                            handledByUI = true;
                            break;
                        }

                        // Normal key processing
                        ultralight::KeyEvent keyDownEvent =
                            WinKeyHandler::CreateKeyEvent(ultralight::KeyEvent::kType_RawKeyDown, wParam, lParam);
                        {
                            std::lock_guard lock(g_eventQueueMutex);
                            g_eventQueue.emplace_back(keyDownEvent);
                        }
                        handledByUI = true;
                        break;
                    }
                    case WM_KEYUP: {
                        ultralight::KeyEvent ev =
                            WinKeyHandler::CreateKeyEvent(ultralight::KeyEvent::kType_KeyUp, wParam, lParam);
                        {
                            std::lock_guard lock(g_eventQueueMutex);
                            g_eventQueue.emplace_back(ev);
                        }
                        handledByUI = true;
                        break;
                    }
                    case WM_CHAR: {
                        const bool viewHasInputFieldFocus = g_isFocusedTextInputActive.load();
                        if (viewHasInputFieldFocus) {
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
                        } else {
                            g_pendingHighSurrogate = 0;
                        }
                        break;
                    }
                    case WM_UNICHAR: {
                        if (wParam == UNICODE_NOCHAR) {
                            return TRUE;
                        }

                        const bool viewHasInputFieldFocus = g_isFocusedTextInputActive.load();
                        if (viewHasInputFieldFocus) {
                            handledByUI = true;

                            std::wstring committedText = ConvertCodePointToUtf16(static_cast<UINT>(wParam));
                            if (!committedText.empty() && ShouldQueueChar(committedText[0])) {
                                g_pendingHighSurrogate = 0;
                                QueueCommittedCharEvent(committedText, lParam);
                            }
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

    void Initialize(HWND gameHwnd, SingleThreadExecutor* coreExecutor,
                    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap,
                    std::shared_mutex* viewsMapMutex) {
        g_hWnd = gameHwnd;
        g_ultralightThreadExecutor = coreExecutor;
        g_viewsMap = viewsMap;
        g_viewsMapMutex = viewsMapMutex;
        g_isAnyInputCaptureActive = false;
        g_isFocusedTextInputActive = false;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            g_currentlyFocusedViewId = 0;
        }

        g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;
        g_nativeCursorX.store(0.0f, std::memory_order_relaxed);
        g_nativeCursorY.store(0.0f, std::memory_order_relaxed);

        logger::info("PrismaUI::InputHandler Initialized with HWND: {}", (void*)g_hWnd);

        g_imeHelper.SetCallbacks(
            [](const std::string& s) { return EscapeForJS(s); },
            [](const std::wstring& ws, LPARAM lp) { QueueCommittedCharEvent(ws, lp); },
            [](const wchar_t* p, int len) { return ConvertUtf16ToUtf8(p, len); });
        g_imeHelper.SetContext({g_hWnd, g_viewsMap, g_viewsMapMutex, &g_focusedViewIdMutex,
                                &g_currentlyFocusedViewId, &g_isAnyInputCaptureActive, &g_isFocusedTextInputActive});
        g_imeHelper.SetExecutor(g_ultralightThreadExecutor);
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
            logger::error("Failed to install WndProc subclass. Error: {} (0x{:X})", err, err);
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

            if (!isTextInputFocused && g_ultralightThreadExecutor) {
                g_ultralightThreadExecutor->submit([viewId]() {
                    g_imeHelper.ClearStateInJS(viewId);
                });
            } else if (!isTextInputFocused) {
                g_imeHelper.ClearStateInJS(viewId);
            }
        });
        InstallImeFocusTrackingForView(viewId);

        g_mouseButtonStates[0] = g_mouseButtonStates[1] = g_mouseButtonStates[2] = false;

        SyncNativeCursorFromMenuCursor();
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

                if (g_ultralightThreadExecutor && currentFocusedBeforeDisable != 0) {
                    g_ultralightThreadExecutor->submit([viewId_copy = currentFocusedBeforeDisable]() {
                        std::shared_ptr<Core::PrismaView> targetViewData = nullptr;
                        {
                            std::shared_lock lock(*g_viewsMapMutex);
                            auto it = g_viewsMap->find(viewId_copy);
                            if (it != g_viewsMap->end()) {
                                targetViewData = it->second;
                            }
                        }

                        if (targetViewData && targetViewData->ultralightView) {
                            logger::debug("Resetting mouse position to (0,0) for View [{}]", viewId_copy);
                            ultralight::MouseEvent resetEvent;
                            resetEvent.type = ultralight::MouseEvent::kType_MouseMoved;
                            resetEvent.x = 0;
                            resetEvent.y = 0;
                            resetEvent.button = ultralight::MouseEvent::kButton_None;

                            targetViewData->ultralightView->FireMouseEvent(resetEvent);
                        }
                    });
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
        if (!g_ultralightThreadExecutor || !g_viewsMap || !g_viewsMapMutex) return;

        Core::PrismaViewId focusedViewIdCopy;
        {
            std::lock_guard lock(g_focusedViewIdMutex);
            focusedViewIdCopy = g_currentlyFocusedViewId;
        }

        if (focusedViewIdCopy == 0 && !g_eventQueue.empty()) {
            std::lock_guard lock(g_eventQueueMutex);
            g_eventQueue.clear();
            return;
        }
        if (focusedViewIdCopy == 0) return;

        std::vector<InputEvent> eventsToProcess;
        {
            std::lock_guard lock(g_eventQueueMutex);
            if (g_eventQueue.empty()) return;
            eventsToProcess.swap(g_eventQueue);
        }

        g_ultralightThreadExecutor->submit([viewId_copy = focusedViewIdCopy, ev_queue = std::move(eventsToProcess)]() {
            std::shared_ptr<Core::PrismaView> targetViewData = nullptr;
            {
                std::shared_lock lock(*g_viewsMapMutex);
                auto it = g_viewsMap->find(viewId_copy);
                if (it != g_viewsMap->end()) {
                    targetViewData = it->second;
                }
            }

            if (targetViewData && targetViewData->ultralightView) {
                ultralight::View* ulView = targetViewData->ultralightView.get();
                ultralight::View* inspectorView =
                    targetViewData->inspectorView ? targetViewData->inspectorView.get() : nullptr;

                for (const auto& event_variant : ev_queue) {
                    std::visit(
                        [ulView, inspectorView, &targetViewData](const auto& arg) {
                            using T = std::decay_t<decltype(arg)>;
                            if constexpr (std::is_same_v<T, ultralight::MouseEvent>) {
                                // Check if mouse is over inspector bounds when inspector is visible
                                bool mouseOverInspector = false;
                                if (inspectorView && targetViewData->inspectorVisible.load()) {
                                    const float inspX = targetViewData->inspectorPosX;
                                    const float inspY = targetViewData->inspectorPosY;
                                    const float inspW = static_cast<float>(targetViewData->inspectorDisplayWidth);
                                    const float inspH = static_cast<float>(targetViewData->inspectorDisplayHeight);

                                    const float mouseX = static_cast<float>(arg.x);
                                    const float mouseY = static_cast<float>(arg.y);

                                    if (mouseX >= inspX && mouseX < (inspX + inspW) && mouseY >= inspY &&
                                        mouseY < (inspY + inspH)) {
                                        mouseOverInspector = true;
                                        targetViewData->inspectorPointerHover.store(true);
                                    } else {
                                        targetViewData->inspectorPointerHover.store(false);
                                    }
                                }

                                if (mouseOverInspector) {
                                    // Translate mouse coordinates to inspector view
                                    ultralight::MouseEvent inspectorEvent = arg;
                                    inspectorEvent.x = arg.x - static_cast<int>(targetViewData->inspectorPosX);
                                    inspectorEvent.y = arg.y - static_cast<int>(targetViewData->inspectorPosY);
                                    inspectorView->FireMouseEvent(inspectorEvent);
                                } else {
                                    ulView->FireMouseEvent(arg);
                                }
                            } else if constexpr (std::is_same_v<T, ScrollEventWithPosition>) {
                                // Route scroll events to inspector if mouse is over it
                                // Use captured mouse position for accurate bounds checking
                                bool scrollOverInspector = false;
                                if (inspectorView && targetViewData->inspectorVisible.load()) {
                                    const float inspX = targetViewData->inspectorPosX;
                                    const float inspY = targetViewData->inspectorPosY;
                                    const float inspW = static_cast<float>(targetViewData->inspectorDisplayWidth);
                                    const float inspH = static_cast<float>(targetViewData->inspectorDisplayHeight);

                                    const float mouseX = static_cast<float>(arg.mouseX);
                                    const float mouseY = static_cast<float>(arg.mouseY);

                                    if (mouseX >= inspX && mouseX < (inspX + inspW) && mouseY >= inspY &&
                                        mouseY < (inspY + inspH)) {
                                        scrollOverInspector = true;
                                    }
                                }
                                if (scrollOverInspector) {
                                    inspectorView->FireScrollEvent(arg.event);
                                } else {
                                    ulView->FireScrollEvent(arg.event);
                                }
                            } else if constexpr (std::is_same_v<T, ultralight::KeyEvent>) {
                                // Route keyboard events to inspector if it's visible and focused
                                if (inspectorView && targetViewData->inspectorVisible.load() &&
                                    inspectorView->HasFocus()) {
                                    inspectorView->FireKeyEvent(arg);
                                } else {
                                    ulView->FireKeyEvent(arg);
                                }
                            }
                        },
                        event_variant);
                }
            }
        });
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
        g_ultralightThreadExecutor = nullptr;
        g_viewsMap = nullptr;
        g_viewsMapMutex = nullptr;
        g_isFocusedTextInputActive = false;
        logger::info("PrismaUI::InputHandler Shutdown.");
    }
}
