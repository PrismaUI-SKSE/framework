#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>

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

    private:
        CefRuntime();
        ~CefRuntime();

        CefRuntime(const CefRuntime&) = delete;
        CefRuntime& operator=(const CefRuntime&) = delete;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
