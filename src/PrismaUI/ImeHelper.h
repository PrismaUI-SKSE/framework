#pragma once

#include "Core.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>

class SingleThreadExecutor;

namespace PrismaUI {

// Callbacks ImeHelper needs from InputHandler (avoids circular dependency)
using ImeEscapeForJSCallback = std::function<std::string(const std::string&)>;
using ImeQueueCommittedCharCallback = std::function<void(const std::wstring&, LPARAM)>;
using ImeConvertUtf16ToUtf8Callback = std::function<std::string(const wchar_t*, int)>;

// Context references for focus/IME state checks
struct ImeHelperContext {
    HWND hwnd = nullptr;
    std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap = nullptr;
    std::shared_mutex* viewsMapMutex = nullptr;
    std::mutex* focusedViewIdMutex = nullptr;
    Core::PrismaViewId* currentlyFocusedViewId = nullptr;
    std::atomic<bool>* isAnyInputCaptureActive = nullptr;
    std::atomic<bool>* isTextInputFocused = nullptr;
};

// Custom IME composition support: reads Windows IME state and dispatches to JS
// for custom candidate/composition UI (hides native IME windows in fullscreen).
class ImeHelper {
public:
    ImeHelper() = default;
    ~ImeHelper() = default;

    // Initialize IME context. Must be called on main thread with valid hwnd.
    void Initialize(HWND hwnd);

    // Release IME context and disassociate from window. Call before hwnd becomes invalid.
    void Shutdown(HWND hwnd);

    // Set callbacks and context. Call after Initialize, before any other operations.
    void SetCallbacks(ImeEscapeForJSCallback escapeForJS, ImeQueueCommittedCharCallback queueCommittedChar,
                      ImeConvertUtf16ToUtf8Callback convertUtf16ToUtf8);
    void SetContext(const ImeHelperContext& ctx);
    void SetExecutor(SingleThreadExecutor* executor);

    // Associate/unassociate IME context with window. Must run on main thread for ImmAssociateContext.
    void SetAssociation(bool enabled);

    // Update IME state based on focused view and text input focus. Call from ultralight thread
    // or via executor.
    void UpdateStateForFocusedView(Core::PrismaViewId viewId);

    // Send current IME composition/candidate state to JS, or clear it.
    void SendStateToJS(Core::PrismaViewId viewId, HWND hwnd, bool active);
    void ClearStateInJS(Core::PrismaViewId viewId);

    // Returns true if IME is currently associated with the window.
    bool IsAssociated() const { return m_associated.load(); }

    // Handle WM_IME_* in SubclassProc. Returns true if message was consumed (handled).
    bool HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                       Core::PrismaViewId focusedViewId, bool* outHandled);

    // Handle internal control messages that must run on the window thread.
    bool HandleControlMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* outResult);

    // Modify lParam for WM_IME_SETCONTEXT (suppress native IME UI). Call before DefSubclassProc.
    void ModifySetContextLParam(LPARAM* lParam, UINT uMsg);

    // Returns human-readable name for IME message type.
    static const char* MessageName(UINT uMsg);

private:
    void DispatchScriptToView(Core::PrismaViewId viewId, const std::string& script);
    bool IsTextInputFocused() const;
    void UpdateStateImpl(Core::PrismaViewId viewId);

    HIMC m_context = nullptr;
    bool m_contextOwned = false;
    std::atomic<bool> m_associated{false};
    std::atomic<bool> m_lastKnownTextInputFocus{false};

    ImeEscapeForJSCallback m_escapeForJS;
    ImeQueueCommittedCharCallback m_queueCommittedChar;
    ImeConvertUtf16ToUtf8Callback m_convertUtf16ToUtf8;
    ImeHelperContext m_ctx;
    SingleThreadExecutor* m_executor = nullptr;
};

}  // namespace PrismaUI
