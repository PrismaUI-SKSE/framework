#include "Cef/Browser/OverlayTexture.h"

#include <utility>

#include "PCH.h"
#include "Utils/D3DTextureCopy.h"

namespace PrismaUI::Cef {
    namespace {
        // The wait below only ever spans one frame of queued GPU work. Anything past
        // this means the GPU or the present loop is wedged; time out rather than pin
        // the CEF UI thread, which also drains PostToCefUi work such as input.
        constexpr DWORD CopyFenceWaitTimeoutMs = 100;
    }

    OverlayTexture::~OverlayTexture() {
        if (_copyCompleteEvent) {
            CloseHandle(_copyCompleteEvent);
            _copyCompleteEvent = nullptr;
        }
    }

    bool OverlayTexture::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        std::lock_guard lock(_mutex);

        if (!device || !context) {
            return false;
        }

        if (_renderDevice.Get() != device) {
            _renderDevice = device;
            _renderDevice1.Reset();
            _copyFence.Reset();
            const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(_renderDevice1.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                logger::error("CEF accelerated OSR disabled: D3D device does not expose ID3D11Device1. HR={:#X}",
                              static_cast<unsigned int>(hr));
                return false;
            }
        }

        if (_renderContext.Get() != context) {
            _renderContext = context;
            _renderContext4.Reset();
            // Optional: absence only costs the render thread the blocking fallback.
            static_cast<void>(context->QueryInterface(IID_PPV_ARGS(_renderContext4.ReleaseAndGetAddressOf())));
        }

        EnsureCopyFenceLocked();

        return true;
    }

    bool OverlayTexture::CreateAcceleratedResourcesLocked(const Desc& desc,
                                                          Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                                          Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) const {
        if (!_renderDevice) {
            return false;
        }

        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = desc.width;
        textureDesc.Height = desc.height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = desc.format;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
        HRESULT hr = _renderDevice->CreateTexture2D(&textureDesc, nullptr, newTexture.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create accelerated CEF overlay texture {}x{} format {}. HR={:#X}",
                          textureDesc.Width, textureDesc.Height, static_cast<unsigned int>(textureDesc.Format),
                          static_cast<unsigned int>(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSrv;
        hr = _renderDevice->CreateShaderResourceView(newTexture.Get(), nullptr, newSrv.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create accelerated CEF overlay SRV {}x{} format {}. HR={:#X}", textureDesc.Width,
                          textureDesc.Height, static_cast<unsigned int>(textureDesc.Format),
                          static_cast<unsigned int>(hr));
            return false;
        }

        texture = std::move(newTexture);
        srv = std::move(newSrv);
        return true;
    }

    void OverlayTexture::EnsureCopyFenceLocked() {
        if (_copyFence || !_renderDevice) {
            return;
        }

        if (!_renderContext4) {
            logger::warn(
                "ID3D11DeviceContext4 unavailable; CEF overlay copies will block the render thread on an event query.");
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        HRESULT hr = _renderDevice->QueryInterface(IID_PPV_ARGS(device5.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            logger::warn(
                "ID3D11Device5 unavailable (HR={:#X}); CEF overlay copies will block the render thread on an event "
                "query.",
                static_cast<unsigned int>(hr));
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11Fence> fence;
        hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            logger::warn("CreateFence failed (HR={:#X}); CEF overlay copies will block the render thread.",
                         static_cast<unsigned int>(hr));
            return;
        }

        if (!_copyCompleteEvent) {
            // Manual reset: the render thread clears it before arming, so a wait can
            // never consume a signal left over from an earlier paint.
            _copyCompleteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!_copyCompleteEvent) {
                logger::warn("CreateEvent failed (GLE={}); CEF overlay copies will block the render thread.",
                             GetLastError());
                return;
            }
        }

        _copyFence = std::move(fence);
        _lastCopyFenceValue = 0;
        logger::info("CEF overlay copies hand completion off to the CEF UI thread through a D3D11 fence.");
    }

    void OverlayTexture::SubmitAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle) {
        if (!sharedTextureHandle) {
            return;
        }

        {
            auto pending = _pendingFrame.Acquire();
            pending->sharedTextureHandle = sharedTextureHandle;
            pending->copyFenceValue = 0;
        }
        _waitForRenderThreadCopyEvent.acquire();

        std::uint64_t copyFenceValue;
        {
            auto pending = _pendingFrame.Acquire();
            copyFenceValue = pending->copyFenceValue;
            pending->copyFenceValue = 0;
        }

        if (copyFenceValue != 0) {
            WaitForCopyCompletion(copyFenceValue);
        }
    }

    void OverlayTexture::WaitForCopyCompletion(std::uint64_t copyFenceValue) {
        if (!_copyCompleteEvent) {
            return;
        }

        if (WaitForSingleObject(_copyCompleteEvent, CopyFenceWaitTimeoutMs) == WAIT_OBJECT_0) {
            return;
        }

        // Returning now lets CEF recycle a texture the GPU may still be reading.
        const std::uint64_t count = ++_copyFenceTimeouts;
        if (count == 1 || count % 300 == 0) {
            logger::warn("CEF overlay copy fence wait timed out after {}ms (value {}, timeout #{}).",
                         CopyFenceWaitTimeoutMs, copyFenceValue, count);
        }
    }

    bool OverlayTexture::CopyPendingAcceleratedFrame() {
        HANDLE textureHandle;
        {
            auto pending = _pendingFrame.Acquire();
            textureHandle = pending->sharedTextureHandle;
            pending->sharedTextureHandle = nullptr;
        }

        if (!textureHandle) {
            return false;
        }

        // Once the handle is taken, the CEF UI thread is committed to waiting on this
        // semaphore; every exit path below must hand it back or CEF wedges for good.
        struct HandoffGuard {
            std::binary_semaphore& semaphore;
            ~HandoffGuard() { semaphore.release(); }
        } handoffGuard{_waitForRenderThreadCopyEvent};

        std::uint64_t copyFenceValue = 0;
        bool copied;
        {
            std::lock_guard lock(_mutex);
            copied = CopySharedHandleOnRenderThreadLocked(textureHandle, copyFenceValue);
        }

        {
            auto pending = _pendingFrame.Acquire();
            pending->copyFenceValue = copied ? copyFenceValue : 0;
        }

        return copied;
    }

    bool OverlayTexture::EnqueueFencedCopyLocked(ID3D11Resource* destination, ID3D11Resource* source,
                                                 std::uint64_t& copyFenceValue) {
        _renderContext->CopyResource(destination, source);

        const std::uint64_t fenceValue = _lastCopyFenceValue + 1;
        ResetEvent(_copyCompleteEvent);

        // Signal submits the context's pending commands, so on a deep queue this is
        // the priciest step here - but it is a submit, not a wait for the GPU.
        HRESULT hr = _renderContext4->Signal(_copyFence.Get(), fenceValue);
        if (SUCCEEDED(hr)) {
            // Non-blocking: this only registers the event with the fence.
            hr = _copyFence->SetEventOnCompletion(fenceValue, _copyCompleteEvent);
        }

        if (FAILED(hr)) {
            logger::error("Failed to arm the CEF overlay copy fence. HR={:#X}", static_cast<unsigned int>(hr));
            return false;
        }

        // Belt and braces: Signal already submits on the drivers measured here, but the
        // fence must be reachable without waiting for the game's own Present, or the CEF
        // UI thread stays blocked for a whole frame. Flush only submits; it does not wait.
        _renderContext->Flush();

        _lastCopyFenceValue = fenceValue;
        copyFenceValue = fenceValue;
        return true;
    }

    bool OverlayTexture::CopySharedHandleOnRenderThreadLocked(HANDLE sharedTextureHandle,
                                                              std::uint64_t& copyFenceValue) {
        copyFenceValue = 0;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
        auto hr = _renderDevice1->OpenSharedResource1(sharedTextureHandle,
                                                      IID_PPV_ARGS(sharedTexture.ReleaseAndGetAddressOf()));

        if (FAILED(hr)) {
            logger::error("Failed to open CEF accelerated shared texture. HR={:#X}.", static_cast<unsigned int>(hr));
            return false;
        }

        D3D11_TEXTURE2D_DESC sharedDesc = {};
        sharedTexture->GetDesc(&sharedDesc);
        if (sharedDesc.Width == 0 || sharedDesc.Height == 0 || sharedDesc.Format == DXGI_FORMAT_UNKNOWN) {
            logger::error("CEF accelerated shared texture had invalid description {}x{} format {}.", sharedDesc.Width,
                          sharedDesc.Height, static_cast<unsigned int>(sharedDesc.Format));
            return false;
        }

        const Desc incoming{sharedDesc.Width, sharedDesc.Height, sharedDesc.Format};
        const bool overlayMatches = texture_ && _srv && _desc.Matches(sharedDesc);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture = texture_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> targetSrv = _srv;
        if (!overlayMatches && !CreateAcceleratedResourcesLocked(incoming, targetTexture, targetSrv)) {
            return false;
        }

        bool fenced = false;
        if (_copyFence) {
            fenced = EnqueueFencedCopyLocked(targetTexture.Get(), sharedTexture.Get(), copyFenceValue);
            if (!fenced) {
                logger::error("Disabling the CEF overlay copy fence; falling back to blocking render-thread copies.");
                _copyFence.Reset();
                copyFenceValue = 0;
            }
        }

        if (!fenced) {
            hr = Utils::CopyResourceAndWait(_renderDevice.Get(), _renderContext.Get(), targetTexture.Get(),
                                            sharedTexture.Get());
            if (FAILED(hr)) {
                logger::error("Failed to synchronously copy CEF accelerated shared texture. HR={:#X}",
                              static_cast<unsigned int>(hr));
                return false;
            }
        }

        sharedTexture.Reset();

        if (!overlayMatches) {
            texture_ = std::move(targetTexture);
            _srv = std::move(targetSrv);
            _desc = incoming;
            logger::info("Created accelerated CEF overlay texture {}x{} DXGI format {}.", _desc.width, _desc.height,
                         static_cast<unsigned int>(_desc.format));
        }

        _hasFrame = true;
        return true;
    }

    void OverlayTexture::ReleaseResources() {
        {
            auto pending = _pendingFrame.Acquire();
            pending->sharedTextureHandle = nullptr;
            pending->copyFenceValue = 0;
        }

        // Free a CEF UI thread parked in either stage of the handoff before the
        // resources it is waiting on go away.
        _waitForRenderThreadCopyEvent.release();
        if (_copyCompleteEvent) {
            SetEvent(_copyCompleteEvent);
        }

        std::lock_guard lock(_mutex);
        if (texture_ || _srv || _renderDevice || _renderContext) {
            logger::info("Releasing CEF overlay D3D resources.");
        }
        _srv.Reset();
        texture_.Reset();
        _copyFence.Reset();
        _renderContext4.Reset();
        _renderContext.Reset();
        _renderDevice1.Reset();
        _renderDevice.Reset();
        _desc = {};
        _hasFrame = false;
    }

    std::optional<OverlayTextureInfo> OverlayTexture::GetInfo() const {
        std::lock_guard lock(_mutex);
        return _hasFrame ? std::make_optional(
                               OverlayTextureInfo{.Srv = _srv.Get(), .Width = _desc.width, .Height = _desc.height})
                         : std::nullopt;
    }

    bool OverlayTexture::HasFrame() const {
        std::lock_guard lock(_mutex);
        return _hasFrame;
    }
}
