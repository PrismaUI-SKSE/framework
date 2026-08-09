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

        InitializeCopyFence();

        return true;
    }

    bool OverlayTexture::CreateAcceleratedResources(const Desc& desc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
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

    void OverlayTexture::InitializeCopyFence() {
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
            _copyCompleteEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!_copyCompleteEvent) {
                logger::warn("CreateEvent failed (GLE={}); CEF overlay copies will block the render thread.",
                             GetLastError());
                return;
            }
        }

        _copyFence = std::move(fence);
        logger::info("CEF overlay copies hand completion off to the CEF UI thread through a D3D11 fence.");
    }

    void OverlayTexture::SubmitAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle) {
        if (!sharedTextureHandle) {
            return;
        }

        uint64_t copyFenceValue;

        {
            auto pending = _pendingFrame.Acquire();
            pending->sharedTextureHandle = sharedTextureHandle;
            ++pending->copyFenceValue;
            copyFenceValue = pending->copyFenceValue;
        }

        if (!WaitForCopyCompletion(copyFenceValue)) {
            auto pending = _pendingFrame.Acquire();
            pending->sharedTextureHandle = nullptr;
        }
    }

    bool OverlayTexture::WaitForCopyCompletion(std::uint64_t copyFenceValue) const {
        if (WaitForSingleObject(_copyCompleteEvent, CopyFenceWaitTimeoutMs) != WAIT_OBJECT_0) {
            logger::warn("CEF overlay copy fence wait timed out after {}ms (value {}, timeout).",
                         CopyFenceWaitTimeoutMs, copyFenceValue);
            return false;
        }

        return true;
    }

    bool OverlayTexture::CopyPendingAcceleratedFrame() {
        HANDLE textureHandle;
        std::uint64_t copyFenceValue;

        {
            auto pending = _pendingFrame.Acquire();
            textureHandle = pending->sharedTextureHandle;
            copyFenceValue = pending->copyFenceValue;
            pending->sharedTextureHandle = nullptr;
        }

        if (!textureHandle) {
            return false;
        }

        return CopySharedHandleOnRenderThread(textureHandle, copyFenceValue);
    }

    bool OverlayTexture::CopySharedHandleOnRenderThread(HANDLE sharedTextureHandle, uint64_t copyFenceValue) {
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
        const bool overlayMatches = _texture && _srv && _desc.Matches(sharedDesc);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture = _texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> targetSrv = _srv;
        if (!overlayMatches && !CreateAcceleratedResources(incoming, targetTexture, targetSrv)) {
            return false;
        }

        bool fenced = false;
        if (_copyFence) {
            fenced = EnqueueFencedCopy(targetTexture.Get(), sharedTexture.Get(), copyFenceValue);
            if (!fenced) {
                logger::error("Disabling the CEF overlay copy fence; falling back to blocking render-thread copies.");
                _copyFence.Reset();
            }
        }

        if (!fenced) {
            hr = Utils::CopyResourceAndWait(_renderDevice.Get(), _renderContext.Get(), targetTexture.Get(),
                                            sharedTexture.Get());
            SetEvent(_copyCompleteEvent);
            if (FAILED(hr)) {
                logger::error("Failed to synchronously copy CEF accelerated shared texture. HR={:#X}",
                              static_cast<unsigned int>(hr));
                return false;
            }
        }

        if (!overlayMatches) {
            _texture = std::move(targetTexture);
            _srv = std::move(targetSrv);
            _desc = incoming;
            logger::info("Created accelerated CEF overlay texture {}x{} DXGI format {}.", _desc.width, _desc.height,
                         static_cast<unsigned int>(_desc.format));
        }

        _hasFrame = true;
        return true;
    }

    bool OverlayTexture::EnqueueFencedCopy(ID3D11Resource* destination, ID3D11Resource* source,
                                           uint64_t copyFenceValue) const {
        _renderContext->CopyResource(destination, source);

        HRESULT hr = _renderContext4->Signal(_copyFence.Get(), copyFenceValue);
        if (SUCCEEDED(hr)) {
            hr = _copyFence->SetEventOnCompletion(copyFenceValue, _copyCompleteEvent);
        }

        if (FAILED(hr)) {
            logger::error("Failed to arm the CEF overlay copy fence. HR={:#X}", static_cast<unsigned int>(hr));
            return false;
        }

        _renderContext->Flush();

        return true;
    }

    void OverlayTexture::ReleaseResources() {
        {
            auto pending = _pendingFrame.Acquire();
            pending->sharedTextureHandle = nullptr;
            pending->copyFenceValue = 0;
        }

        if (_copyCompleteEvent) {
            SetEvent(_copyCompleteEvent);
        }

        if (_texture || _srv || _renderDevice || _renderContext) {
            logger::info("Releasing CEF overlay D3D resources.");
        }

        _srv.Reset();
        _texture.Reset();
        _copyFence.Reset();
        _renderContext4.Reset();
        _renderContext.Reset();
        _renderDevice1.Reset();
        _renderDevice.Reset();
        _desc = {};
        _hasFrame = false;
    }

    std::optional<OverlayTextureInfo> OverlayTexture::GetInfo() const {
        return _hasFrame ? std::make_optional(
                               OverlayTextureInfo{.Srv = _srv.Get(), .Width = _desc.width, .Height = _desc.height})
                         : std::nullopt;
    }
}
