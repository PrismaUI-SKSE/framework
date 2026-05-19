#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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
