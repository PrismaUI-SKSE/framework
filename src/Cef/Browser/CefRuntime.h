#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Cef/Shared/RendererToBrowserMessages.h"

namespace PrismaUI::Cef {
    enum class CefInputMouseButton : uint8_t { Left, Middle, Right };

    enum class CefInputKeyType : uint8_t { RawKeyDown, KeyUp, Char };

    struct CefInputMouseMove {
        int x = 0;
        int y = 0;
        uint32_t modifiers = 0;
        bool mouseLeave = false;
    };

    struct CefInputMouseClick {
        int x = 0;
        int y = 0;
        uint32_t modifiers = 0;
        CefInputMouseButton button = CefInputMouseButton::Left;
        bool mouseUp = false;
        int clickCount = 1;
    };

    struct CefInputMouseWheel {
        int x = 0;
        int y = 0;
        uint32_t modifiers = 0;
        int deltaX = 0;
        int deltaY = 0;
    };

    struct CefInputKey {
        CefInputKeyType type = CefInputKeyType::RawKeyDown;
        uint32_t modifiers = 0;
        int windowsKeyCode = 0;
        int nativeKeyCode = 0;
        char16_t character = 0;
        char16_t unmodifiedCharacter = 0;
        bool isSystemKey = false;
        bool focusOnEditableField = false;
    };

    using CefInputEvent = std::variant<CefInputMouseMove, CefInputMouseClick, CefInputMouseWheel, CefInputKey>;

    class CefRuntime final {
    public:
        static CefRuntime& GetSingleton();

        bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
        void Resize(uint32_t width, uint32_t height);
        void BeginFrame();
        void UpdateOverlayTexture(ID3D11Device* device, ID3D11DeviceContext* context);
        ID3D11ShaderResourceView* GetOverlaySrv() const;
        uint32_t GetOverlayWidth() const;
        uint32_t GetOverlayHeight() const;
        bool CopyAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle);
        void Shutdown();
        bool IsInitialized() const;
        bool HasBrowser() const;
        bool CreateShellView(uint64_t viewId, std::string_view urlOrPath, int order, bool hidden);
        bool DestroyShellView(uint64_t viewId);
        bool SetShellViewHidden(uint64_t viewId, bool hidden);
        bool SetShellViewOrder(uint64_t viewId, int order);
        bool FocusShellView(uint64_t viewId);
        bool BlurShellView(uint64_t viewId);
        bool TryGetShellFrameName(uint64_t viewId, std::string& outName) const;
        bool IsShellReady() const;
        void OpenDevTools();
        void CloseDevTools();
        bool IsDevToolsOpen() const;

        // Dispatch a batch of native input events to the focused CEF OSR shell iframe.
        // The events are copied into a single CEF UI-thread task; callers must not call
        // CefBrowserHost input APIs directly from game, window, or render callbacks.
        void DispatchInputEvents(uint64_t viewId, std::vector<CefInputEvent> events);

        void NotifyShellLoadStart(const std::string& frameIdentifier, const std::string& url);
        void NotifyShellLoadEnd(int httpStatusCode, const std::string& frameIdentifier, const std::string& url);
        void NotifyShellLoadError(int errorCode, const std::string& errorText, const std::string& failedUrl,
                                  const std::string& frameIdentifier, const std::string& url);
        void NotifyShellFrameLoadStart(const std::string& frameName, const std::string& frameIdentifier,
                                       const std::string& url);
        void NotifyShellFrameLoadEnd(const std::string& frameName, const std::string& frameIdentifier,
                                     const std::string& url, int httpStatusCode);
        void NotifyShellFrameLoadError(const std::string& frameName, const std::string& frameIdentifier,
                                       const std::string& url, int errorCode, const std::string& errorText,
                                       const std::string& failedUrl);

        // ---- JavaScript bridge surface (Step 7) ----
        // Eval `script` inside the iframe associated with `viewId`. `callback` (if any)
        // is invoked exactly once with the coerced string result. On failure (view
        // unknown, frame missing, JS exception, view destroyed mid-flight) `callback`
        // fires with an empty string.
        void InvokeScript(uint64_t viewId, std::string script, std::function<void(std::string)> callback);

        // Forward a (functionName, argument) tuple to the iframe's window[functionName].
        // Fire-and-forget: missing target frame or non-callable property are logged but
        // do not surface to the caller.
        void InteropCallInView(uint64_t viewId, std::string functionName, std::string argument);

        // Register a (viewId, name) -> SimpleJSCallback. The renderer installs a
        // window[name] trampoline that forwards to this callback the next time the
        // matching frame's V8 context is created. Re-registering replaces the prior
        // callback in place.
        void RegisterListener(uint64_t viewId, std::string name, std::function<void(const std::string&)> callback);

        // Drop any pending Invoke requests issued against `viewId`, firing their
        // callbacks with an empty string. Called from ViewManager::Destroy so the
        // caller is never left holding a never-fired callback.
        void CancelInvokesForView(uint64_t viewId);

        // Routes a process message received from a renderer subprocess to the
        // matching listener / Invoke / console / DOM-ready dispatcher. Returns true
        // if the message was recognised.
        bool OnRendererMessage(const CefString& frameName, const RTBMessages::RendererToBrowserMessage& message);

    private:
        CefRuntime();
        ~CefRuntime();

        CefRuntime(const CefRuntime&) = delete;
        CefRuntime& operator=(const CefRuntime&) = delete;

        struct ShellCreateViewArg {
            uint64_t id;
            std::string_view url;
            int order;
            bool hidden;
        };

        static void AppendShellArg(std::string& out, uint64_t value);
        static void AppendShellArg(std::string& out, int value);
        static void AppendShellArg(std::string& out, bool value);
        static void AppendShellArg(std::string& out, std::string_view value);
        static void AppendShellArg(std::string& out, const ShellCreateViewArg& value);

        bool RunShellScript(std::string_view method, uint64_t viewId, std::string script);

        // Build `window.__prismaShell.<method>(<args>);` from typed arguments and
        // dispatch it through `RunShellScript`. If the caller is already on the CEF
        // UI thread the script runs inline; otherwise it is posted to TID_UI.
        // `viewId` is used for iframe-existence diagnostics and logging; pass the
        // affected view's id (createView is exempted from the iframe check).
        template <class... Args>
        bool InvokeShell(std::string_view method, uint64_t viewId, const Args&... args) {
            std::string script;
            script.reserve(48 + method.size());
            script += "window.__prismaShell.";
            script.append(method);
            script += '(';
            bool first = true;
            const auto appendOne = [&](const auto& value) {
                if (!first) {
                    script += ", ";
                }
                first = false;
                AppendShellArg(script, value);
            };
            (appendOne(args), ...);
            script += ");";
            return RunShellScript(method, viewId, std::move(script));
        }
        void ReplayShellViews();

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
