#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace PrismaUI::Cef {
    // Owns the D3D11 surface that backs CEF's OSR output (accelerated shared-texture
    // copy and CPU OnPaint fallback). The browser-process CefRuntime hands it raw
    // device/context pointers and either a CEF shared HANDLE (accelerated) or a BGRA32
    // pixel buffer (CPU); the overlay produces an ID3D11ShaderResourceView the render
    // path samples each frame.
    //
    // Thread model: every public method takes an internal mutex. Callers are still
    // responsible for invoking them from a thread that is legal for D3D11 work --
    // resource creation/upload from the render (D3D Present) thread, and accelerated
    // shared-handle copies from the CEF UI thread inside OnAcceleratedPaint.
    class OverlayTexture final {
    public:
        enum class Mode : std::uint8_t { None, Accelerated, Cpu };

        OverlayTexture() = default;
        ~OverlayTexture() = default;

        OverlayTexture(const OverlayTexture&) = delete;
        OverlayTexture& operator=(const OverlayTexture&) = delete;

        // Render-thread: bind (or refresh) the D3D device and context. Idempotent;
        // safe to call every frame. Reconfigures ID3D11Multithread protection on the
        // first call after the context pointer changes.
        void BindRenderDevice(ID3D11Device* device, ID3D11DeviceContext* context);

        // Render-thread: if a previous CopyFromSharedHandle observed an accelerated
        // description that did not match the current overlay, create the matching
        // texture and SRV now. Returns true exactly when a new accelerated texture
        // was created during this call (so the caller can ask CEF to repaint into
        // it).
        bool RealizePendingAccelerated();

        // CEF UI thread (OnAcceleratedPaint): copy the CEF shared texture into the
        // overlay. Returns true on a successful copy. On size/format mismatch, records
        // a pending request so the next RealizePendingAccelerated reallocates, and
        // returns false.
        bool CopyFromSharedHandle(HANDLE sharedTextureHandle);

        // Render-thread: upload BGRA32 pixels into a CPU-mode overlay. Recreates the
        // underlying texture when dimensions or mode change. `srcStride` is the byte
        // stride of the source buffer and must be >= width * 4. Returns true on
        // success.
        bool UploadBgra32(const std::byte* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t srcStride);

        // Render-thread: drop the texture, SRV, and cached device/context references.
        void ReleaseResources();

        ID3D11ShaderResourceView* GetSrv() const;
        std::uint32_t GetWidth() const;
        std::uint32_t GetHeight() const;
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

        void EnsureMultithreadProtectedLocked();
        bool AllocateAcceleratedLocked(const Desc& desc);
        bool AllocateCpuLocked(const Desc& desc);
        void NoteActiveModeChangeLocked(Mode newMode);

        static const char* ModeName(Mode mode);

        mutable std::mutex mutex_;

        Microsoft::WRL::ComPtr<ID3D11Device> renderDevice_;
        Microsoft::WRL::ComPtr<ID3D11Device1> renderDevice1_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> renderContext_;
        Microsoft::WRL::ComPtr<ID3D11Multithread> d3dMultithread_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;

        Desc desc_;
        Desc pendingAcceleratedDesc_;
        Mode mode_ = Mode::None;
        Mode activeMode_ = Mode::None;
        bool pendingAccelerated_ = false;
        bool hasFrame_ = false;
        bool multithreadProtectionConfigured_ = false;
        bool multithreadProtectionUnavailableLogged_ = false;
        bool bridgeUnavailableLogged_ = false;
        bool firstAcceleratedCopyLogged_ = false;
    };
}
