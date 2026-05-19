#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace PrismaUI::Cef
{
    class CefRuntime final
    {
    public:
        static CefRuntime& GetSingleton();

        bool Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height);
        void Resize(uint32_t width, uint32_t height);
        void BeginFrame();
        void UpdateOverlayTexture(ID3D11Device* device, ID3D11DeviceContext* context);
        ID3D11ShaderResourceView* GetOverlaySrv() const;
        uint32_t GetOverlayWidth() const;
        uint32_t GetOverlayHeight() const;
        void ReleaseRenderResources();
        bool CopyAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle);
        void Shutdown();
        bool IsInitialized() const;
        bool HasBrowser() const;
        void PostToCefUi(std::function<void()> task);
        bool CreateShellView(uint64_t viewId, std::string_view urlOrPath, int order, bool hidden);
        bool DestroyShellView(uint64_t viewId);
        bool SetShellViewHidden(uint64_t viewId, bool hidden);
        bool SetShellViewOrder(uint64_t viewId, int order);
        bool FocusShellView(uint64_t viewId);
        bool BlurShellView(uint64_t viewId);
        bool TryGetShellFrameName(uint64_t viewId, std::string& outName) const;
        bool IsShellReady() const;

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
        void InvokeScript(uint64_t viewId, std::string script,
                          std::function<void(std::string)> callback);

        // Forward a (functionName, argument) tuple to the iframe's window[functionName].
        // Fire-and-forget: missing target frame or non-callable property are logged but
        // do not surface to the caller.
        void InteropCallInView(uint64_t viewId, std::string functionName, std::string argument);

        // Register a (viewId, name) -> SimpleJSCallback. The renderer installs a
        // window[name] trampoline that forwards to this callback the next time the
        // matching frame's V8 context is created. Re-registering replaces the prior
        // callback in place.
        void RegisterListener(uint64_t viewId, std::string name,
                              std::function<void(const std::string&)> callback);

        // Drop any pending Invoke requests issued against `viewId`, firing their
        // callbacks with an empty string. Called from ViewManager::Destroy so the
        // caller is never left holding a never-fired callback.
        void CancelInvokesForView(uint64_t viewId);

        // Routes a process message received from a renderer subprocess to the
        // matching listener / Invoke / console / DOM-ready dispatcher. Returns true
        // if the message was recognised.
        bool OnRendererMessage(const std::string& frameName, const std::string& messageName,
                               const std::vector<std::string>& payload);

    private:
        CefRuntime();
        ~CefRuntime();

        CefRuntime(const CefRuntime&) = delete;
        CefRuntime& operator=(const CefRuntime&) = delete;

        bool RunShellCommand(const std::string& command, const std::string& description,
                             const std::string& iframeName = {});
        void ReplayShellViews();

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
