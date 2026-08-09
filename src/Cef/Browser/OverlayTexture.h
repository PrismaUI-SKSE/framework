#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <semaphore>

#include "Utils/ResourceLock.h"

namespace PrismaUI::Cef {
    struct OverlayTextureInfo {
        ID3D11ShaderResourceView* Srv;
        std::uint32_t Width;
        std::uint32_t Height;
    };

    // Owns PrismaUI's stable D3D11 surface for CEF OSR output. Accelerated CEF
    // frames are copied from CEF's callback-scoped shared texture into this
    // overlay. The render path samples only this object's SRV.
    //
    // Thread model: Initialize, CopyPendingAcceleratedFrame, and ReleaseResources
    // are render-thread methods. SubmitAcceleratedFrameDuringCallback is the only
    // CEF-UI-thread method and performs no D3D context work.
    //
    // CEF recycles the texture behind |sharedTextureHandle| as soon as
    // OnAcceleratedPaint returns, and that texture carries no keyed mutex, so the
    // copy must be known to have retired on the GPU before the callback unwinds.
    // The render thread therefore only *enqueues* the copy and arms a fence; the
    // completion wait happens on the CEF UI thread, which is blocked in the
    // callback regardless. Waiting on the render thread instead would drain the
    // game's entire queued GPU backlog once per paint.
    class OverlayTexture final {
    public:
        OverlayTexture() = default;
        ~OverlayTexture();

        OverlayTexture(const OverlayTexture&) = delete;
        OverlayTexture& operator=(const OverlayTexture&) = delete;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

        // CEF UI thread (OnAcceleratedPaint): publish a callback-scoped CEF shared
        // texture handle, wait until the render thread has enqueued the copy, then
        // wait for that copy to retire on the GPU.
        void SubmitAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle);

        // Render-thread: consume the pending accelerated paint request, if any,
        // reopening CEF's callback-scoped shared texture and enqueueing the copy
        // into PrismaUI's stable overlay texture.
        bool CopyPendingAcceleratedFrame();

        // Render-thread: drop the texture, SRV, and cached device/context references.
        void ReleaseResources();

        std::optional<OverlayTextureInfo> GetInfo() const;

    private:
        struct Desc {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

            bool Matches(const D3D11_TEXTURE2D_DESC& other) const {
                return width == other.Width && height == other.Height && format == other.Format;
            }
        };

        struct PendingFrame {
            HANDLE sharedTextureHandle = nullptr;
            std::uint64_t copyFenceValue = 0;
        };

        void InitializeCopyFence();

        bool WaitForCopyCompletion(std::uint64_t copyFenceValue) const;

        bool CopySharedHandleOnRenderThread(HANDLE sharedTextureHandle, uint64_t copyFenceValue);
        bool CreateAcceleratedResources(const Desc& desc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) const;
        bool EnqueueFencedCopy(ID3D11Resource* destination, ID3D11Resource* source, uint64_t copyFenceValue) const;

        Microsoft::WRL::ComPtr<ID3D11Device> _renderDevice;
        Microsoft::WRL::ComPtr<ID3D11Device1> _renderDevice1;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _renderContext;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> _renderContext4;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _srv;

        Desc _desc;
        bool _hasFrame = false;

        HANDLE _copyCompleteEvent = nullptr;
        Microsoft::WRL::ComPtr<ID3D11Fence> _copyFence;
        ResourceLock<PendingFrame> _pendingFrame{{}};
    };
}
