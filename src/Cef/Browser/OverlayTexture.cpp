#include "Cef/Browser/OverlayTexture.h"

#include <chrono>
#include <cstring>
#include <utility>

#include "PCH.h"
#include "Utils/D3DTextureCopy.h"

namespace PrismaUI::Cef {
    const char* OverlayTexture::ModeName(Mode mode) {
        switch (mode) {
            case Mode::Accelerated:
                return "accelerated shared texture";
            case Mode::Cpu:
                return "CPU OnPaint fallback";
            default:
                return "none";
        }
    }

    void OverlayTexture::BindRenderDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
        std::lock_guard lock(_mutex);

        if (!device || !context) {
            return;
        }

        if (renderDevice_.Get() != device) {
            renderDevice_ = device;
            renderDevice1_.Reset();
            const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(renderDevice1_.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                logger::error("CEF accelerated OSR disabled: D3D device does not expose ID3D11Device1. HR={:#X}",
                              static_cast<unsigned int>(hr));
            }
        }

        if (renderContext_.Get() != context) {
            renderContext_ = context;
        }
    }

    bool OverlayTexture::CreateAcceleratedResourcesLocked(const Desc& desc,
                                                          Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                                          Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {
        if (!renderDevice_) {
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
        HRESULT hr = renderDevice_->CreateTexture2D(&textureDesc, nullptr, newTexture.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create accelerated CEF overlay texture {}x{} format {}. HR={:#X}",
                          textureDesc.Width, textureDesc.Height, static_cast<unsigned int>(textureDesc.Format),
                          static_cast<unsigned int>(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSrv;
        hr = renderDevice_->CreateShaderResourceView(newTexture.Get(), nullptr, newSrv.ReleaseAndGetAddressOf());
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

    bool OverlayTexture::CreateCpuResourcesLocked(const Desc& desc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                                  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {
        if (!renderDevice_) {
            return false;
        }

        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = desc.width;
        textureDesc.Height = desc.height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = desc.format;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DYNAMIC;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
        HRESULT hr = renderDevice_->CreateTexture2D(&textureDesc, nullptr, newTexture.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create CPU fallback CEF overlay texture {}x{}. HR={:#X}", textureDesc.Width,
                          textureDesc.Height, static_cast<unsigned int>(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSrv;
        hr = renderDevice_->CreateShaderResourceView(newTexture.Get(), nullptr, newSrv.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create CPU fallback CEF overlay SRV {}x{}. HR={:#X}", textureDesc.Width,
                          textureDesc.Height, static_cast<unsigned int>(hr));
            return false;
        }

        texture = std::move(newTexture);
        srv = std::move(newSrv);
        return true;
    }

    void OverlayTexture::NoteActiveModeChangeLocked(Mode newMode) {
        if (_activeMode == newMode) {
            return;
        }
        if (newMode == Mode::Cpu) {
            logger::warn("CEF render path switched: {} -> {}.", ModeName(_activeMode), ModeName(newMode));
        } else {
            logger::info("CEF render path switched: {} -> {}.", ModeName(_activeMode), ModeName(newMode));
        }
        _activeMode = newMode;
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
        HRESULT hr = renderDevice1_->OpenSharedResource1(sharedTextureHandle,
                                                         IID_PPV_ARGS(sharedTexture.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            logger::error("Failed to open CEF accelerated shared texture. HR={:#X}", static_cast<unsigned int>(hr));
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
        const bool overlayMatches = texture_ && _srv && _mode == Mode::Accelerated && _desc.Matches(sharedDesc);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture = texture_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> targetSrv = _srv;
        if (!overlayMatches && !CreateAcceleratedResourcesLocked(incoming, targetTexture, targetSrv)) {
            return false;
        }

        hr = Utils::CopyResourceAndWait(renderDevice_.Get(), renderContext_.Get(), targetTexture.Get(),
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
            _mode = Mode::Accelerated;
            logger::info("Created accelerated CEF overlay texture {}x{} DXGI format {}.", _desc.width, _desc.height,
                         static_cast<unsigned int>(_desc.format));
        }
        _hasFrame = true;
        if (!_firstAcceleratedCopyLogged) {
            logger::info("First accelerated CEF overlay frame copied: {}x{} DXGI format {}.", _desc.width, _desc.height,
                         static_cast<unsigned int>(_desc.format));
            _firstAcceleratedCopyLogged = true;
        }
        NoteActiveModeChangeLocked(Mode::Accelerated);
        return true;
    }

    bool OverlayTexture::UploadBgra32(const std::byte* pixels, std::uint32_t width, std::uint32_t height,
                                      std::uint32_t srcStride) {
        if (!pixels || width == 0 || height == 0) {
            return false;
        }

        std::lock_guard lock(_mutex);
        if (!renderDevice_ || !renderContext_) {
            return false;
        }

        const Desc desc{width, height, DXGI_FORMAT_B8G8R8A8_UNORM};
        const bool overlayMatches = texture_ && _srv && _mode == Mode::Cpu && _desc.width == desc.width &&
                                    _desc.height == desc.height && _desc.format == desc.format;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture = texture_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> targetSrv = _srv;
        if (!overlayMatches && !CreateCpuResourcesLocked(desc, targetTexture, targetSrv)) {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = renderContext_->Map(targetTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            logger::error("Failed to map CPU fallback CEF overlay texture. HR={:#X}", static_cast<unsigned int>(hr));
            return false;
        }

        const std::uint32_t rowBytes = width * 4U;
        auto* destination = static_cast<std::byte*>(mapped.pData);
        for (std::uint32_t row = 0; row < height; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                        pixels + static_cast<std::size_t>(row) * srcStride, rowBytes);
        }
        renderContext_->Unmap(targetTexture.Get(), 0);

        if (!overlayMatches) {
            texture_ = std::move(targetTexture);
            _srv = std::move(targetSrv);
            _desc = desc;
            _mode = Mode::Cpu;
            logger::warn("Created degraded CPU fallback CEF overlay texture {}x{}.", _desc.width, _desc.height);
        }
        _hasFrame = true;
        NoteActiveModeChangeLocked(Mode::Cpu);
        return true;
    }

    void OverlayTexture::ReleaseResources() {
        {
            auto textureHandle = _pendingAcceleratedTextureHandle.Acquire();
            *textureHandle = nullptr;
        }

        _waitForRenderThreadCopyEvent.release();
        std::lock_guard lock(_mutex);
        if (texture_ || _srv || renderDevice_ || renderContext_) {
            logger::info("Releasing CEF overlay D3D resources.");
        }
        _srv.Reset();
        texture_.Reset();
        renderContext_.Reset();
        renderDevice1_.Reset();
        renderDevice_.Reset();
        _desc = {};
        _mode = Mode::None;
        _activeMode = Mode::None;
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

    OverlayTexture::Mode OverlayTexture::GetActiveMode() const {
        std::lock_guard lock(_mutex);
        return _activeMode;
    }
}
