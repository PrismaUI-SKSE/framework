#include "Cef/Browser/OverlayTexture.h"

#include <utility>

#include "PCH.h"
#include "Utils/D3DTextureCopy.h"

namespace PrismaUI::Cef {
    bool OverlayTexture::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        std::lock_guard lock(_mutex);

        if (!device || !context) {
            return false;
        }

        if (_renderDevice.Get() != device) {
            _renderDevice = device;
            _renderDevice1.Reset();
            const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(_renderDevice1.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                logger::error("CEF accelerated OSR disabled: D3D device does not expose ID3D11Device1. HR={:#X}",
                              static_cast<unsigned int>(hr));
                return false;
            }
        }

        if (_renderContext.Get() != context) {
            _renderContext = context;
        }

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

    void OverlayTexture::SubmitAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle) {
        if (!sharedTextureHandle) {
            return;
        }

        {
            auto textureHandle = _pendingAcceleratedTextureHandle.Acquire();
            *textureHandle = sharedTextureHandle;
        }
        _waitForRenderThreadCopyEvent.acquire();
    }

    bool OverlayTexture::CopyPendingAcceleratedFrame() {
        HANDLE textureHandle;
        {
            auto textureHandleLock = _pendingAcceleratedTextureHandle.Acquire();
            textureHandle = *textureHandleLock;
            *textureHandleLock = nullptr;
        }

        if (!textureHandle) {
            return false;
        }

        std::lock_guard lock(_mutex);
        auto copied = CopySharedHandleOnRenderThreadLocked(textureHandle);
        _waitForRenderThreadCopyEvent.release();
        return copied;
    }

    bool OverlayTexture::CopySharedHandleOnRenderThreadLocked(HANDLE sharedTextureHandle) {
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

        hr = Utils::CopyResourceAndWait(_renderDevice.Get(), _renderContext.Get(), targetTexture.Get(),
                                        sharedTexture.Get());
        if (FAILED(hr)) {
            logger::error("Failed to synchronously copy CEF accelerated shared texture. HR={:#X}",
                          static_cast<unsigned int>(hr));
            return false;
        }
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
            auto textureHandle = _pendingAcceleratedTextureHandle.Acquire();
            *textureHandle = nullptr;
        }

        _waitForRenderThreadCopyEvent.release();
        std::lock_guard lock(_mutex);
        if (texture_ || _srv || _renderDevice || _renderContext) {
            logger::info("Releasing CEF overlay D3D resources.");
        }
        _srv.Reset();
        texture_.Reset();
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
