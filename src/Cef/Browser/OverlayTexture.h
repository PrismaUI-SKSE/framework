#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
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
    // overlay; CPU OnPaint fallback uploads BGRA32 pixels into the same sampled
    // surface. The render path samples only this object's SRV.
    //
    // Thread model: BindRenderDevice, CopyPendingAcceleratedFrame, UploadBgra32,
    // and ReleaseResources are render-thread methods. SubmitAcceleratedFrameDuringCallback
    // is the only CEF UI-thread method; it performs no D3D calls and blocks only
    // long enough for the render thread to copy CEF's callback-scoped resource.
    class OverlayTexture final {
    public:
        enum class Mode : std::uint8_t { None, Accelerated, Cpu };

        OverlayTexture() = default;
        ~OverlayTexture() = default;

        OverlayTexture(const OverlayTexture&) = delete;
        OverlayTexture& operator=(const OverlayTexture&) = delete;

        // Render-thread: bind (or refresh) the D3D device and context. Idempotent;
        // safe to call every frame.
        void BindRenderDevice(ID3D11Device* device, ID3D11DeviceContext* context);

        // CEF UI thread (OnAcceleratedPaint): publish a callback-scoped CEF shared
        // texture handle and wait until the render thread copies it into PrismaUI's
        // stable overlay texture. Returns true when that copy succeeds.
        void SubmitAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle);

        // Render-thread: consume the pending accelerated paint request, if any,
        // reopening and copying CEF's callback-scoped shared texture before the CEF
        // callback is allowed to return.
        bool CopyPendingAcceleratedFrame();

        // Render-thread: upload BGRA32 pixels into a CPU-mode overlay. Recreates the
        // underlying texture when dimensions or mode change. `srcStride` is the byte
        // stride of the source buffer and must be >= width * 4. Returns true on
        // success.
        bool UploadBgra32(const std::byte* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t srcStride);

        // Render-thread: drop the texture, SRV, and cached device/context references.
        void ReleaseResources();

        std::optional<OverlayTextureInfo> GetInfo() const;
        bool HasFrame() const;
        Mode GetActiveMode() const;

    private:
        struct Desc {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

            bool Matches(const D3D11_TEXTURE2D_DESC& other) const {
                return width == other.Width && height == other.Height && format == other.Format;
            }
        };

        bool CopySharedHandleOnRenderThreadLocked(HANDLE sharedTextureHandle);
        bool CreateAcceleratedResourcesLocked(const Desc& desc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                              Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);
        bool CreateCpuResourcesLocked(const Desc& desc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);
        void NoteActiveModeChangeLocked(Mode newMode);
        static const char* ModeName(Mode mode);

        mutable std::mutex _mutex;

        Microsoft::WRL::ComPtr<ID3D11Device> renderDevice_;
        Microsoft::WRL::ComPtr<ID3D11Device1> renderDevice1_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> renderContext_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _srv;

        Desc _desc;
        Mode _mode = Mode::None;
        Mode _activeMode = Mode::None;
        bool _hasFrame = false;
        bool _firstAcceleratedCopyLogged = false;

        ResourceLock<HANDLE> _pendingAcceleratedTextureHandle{nullptr};
        std::binary_semaphore _waitForRenderThreadCopyEvent{0};
    };
}
