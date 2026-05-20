#include "PCH.h"

#include "Cef/Browser/OverlayTexture.h"

#include <cstring>
#include <utility>

namespace PrismaUI::Cef
{
    const char* OverlayTexture::ModeName(Mode mode)
    {
        switch (mode) {
            case Mode::Accelerated:
                return "accelerated shared texture";
            case Mode::Cpu:
                return "CPU OnPaint fallback";
            default:
                return "none";
        }
    }

    void OverlayTexture::BindRenderDevice(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        std::lock_guard lock(mutex_);

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
            d3dMultithread_.Reset();
            multithreadProtectionConfigured_ = false;
        }

        EnsureMultithreadProtectedLocked();
    }

    void OverlayTexture::EnsureMultithreadProtectedLocked()
    {
        if (multithreadProtectionConfigured_ || !renderContext_) {
            return;
        }

        const HRESULT hr =
            renderContext_->QueryInterface(IID_PPV_ARGS(d3dMultithread_.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(hr) && d3dMultithread_) {
            const BOOL wasProtected = d3dMultithread_->SetMultithreadProtected(TRUE);
            logger::info("CEF overlay enabled D3D11 multithread protection (previously {}).",
                         wasProtected ? "enabled" : "disabled");
        } else if (!multithreadProtectionUnavailableLogged_) {
            logger::warn("CEF overlay could not acquire ID3D11Multithread; accelerated copies will use only the overlay mutex. HR={:#X}",
                         static_cast<unsigned int>(hr));
            multithreadProtectionUnavailableLogged_ = true;
        }
        multithreadProtectionConfigured_ = true;
    }

    bool OverlayTexture::AllocateAcceleratedLocked(const Desc& desc)
    {
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

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = renderDevice_->CreateTexture2D(&textureDesc, nullptr, texture.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create accelerated CEF overlay texture {}x{} format {}. HR={:#X}",
                          textureDesc.Width, textureDesc.Height, static_cast<unsigned int>(textureDesc.Format),
                          static_cast<unsigned int>(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        hr = renderDevice_->CreateShaderResourceView(texture.Get(), nullptr, srv.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create accelerated CEF overlay SRV {}x{} format {}. HR={:#X}",
                          textureDesc.Width, textureDesc.Height, static_cast<unsigned int>(textureDesc.Format),
                          static_cast<unsigned int>(hr));
            return false;
        }

        texture_ = std::move(texture);
        srv_ = std::move(srv);
        desc_ = desc;
        mode_ = Mode::Accelerated;
        hasFrame_ = false;
        logger::info("Created accelerated CEF overlay texture {}x{} DXGI format {}.", desc_.width, desc_.height,
                     static_cast<unsigned int>(desc_.format));
        return true;
    }

    bool OverlayTexture::AllocateCpuLocked(const Desc& desc)
    {
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

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = renderDevice_->CreateTexture2D(&textureDesc, nullptr, texture.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create CPU fallback CEF overlay texture {}x{}. HR={:#X}", textureDesc.Width,
                          textureDesc.Height, static_cast<unsigned int>(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        hr = renderDevice_->CreateShaderResourceView(texture.Get(), nullptr, srv.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            logger::error("Failed to create CPU fallback CEF overlay SRV {}x{}. HR={:#X}", textureDesc.Width,
                          textureDesc.Height, static_cast<unsigned int>(hr));
            return false;
        }

        texture_ = std::move(texture);
        srv_ = std::move(srv);
        desc_ = desc;
        mode_ = Mode::Cpu;
        hasFrame_ = false;
        logger::warn("Created degraded CPU fallback CEF overlay texture {}x{}.", desc_.width, desc_.height);
        return true;
    }

    void OverlayTexture::NoteActiveModeChangeLocked(Mode newMode)
    {
        if (activeMode_ == newMode) {
            return;
        }
        if (newMode == Mode::Cpu) {
            logger::warn("CEF render path switched: {} -> {}.", ModeName(activeMode_), ModeName(newMode));
        } else {
            logger::info("CEF render path switched: {} -> {}.", ModeName(activeMode_), ModeName(newMode));
        }
        activeMode_ = newMode;
    }

    bool OverlayTexture::RealizePendingAccelerated()
    {
        std::lock_guard lock(mutex_);
        if (!pendingAccelerated_ || pendingAcceleratedDesc_.width == 0 ||
            pendingAcceleratedDesc_.height == 0 || pendingAcceleratedDesc_.format == DXGI_FORMAT_UNKNOWN) {
            return false;
        }

        const bool created = AllocateAcceleratedLocked(pendingAcceleratedDesc_);
        pendingAccelerated_ = false;
        return created;
    }

    bool OverlayTexture::CopyFromSharedHandle(HANDLE sharedTextureHandle)
    {
        if (!sharedTextureHandle) {
            return false;
        }

        std::lock_guard lock(mutex_);
        if (!renderDevice1_ || !renderContext_) {
            if (!bridgeUnavailableLogged_) {
                logger::warn("CEF accelerated paint arrived before the D3D11.1 render bridge was ready.");
                bridgeUnavailableLogged_ = true;
            }
            return false;
        }
        bridgeUnavailableLogged_ = false;

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
            logger::error("CEF accelerated shared texture had invalid description {}x{} format {}.",
                          sharedDesc.Width, sharedDesc.Height, static_cast<unsigned int>(sharedDesc.Format));
            return false;
        }

        const Desc incoming{sharedDesc.Width, sharedDesc.Height, sharedDesc.Format};
        const bool overlayMatches = texture_ && srv_ && mode_ == Mode::Accelerated && desc_.Matches(sharedDesc);
        if (!overlayMatches) {
            const bool pendingMatches = pendingAccelerated_ &&
                                        pendingAcceleratedDesc_.width == incoming.width &&
                                        pendingAcceleratedDesc_.height == incoming.height &&
                                        pendingAcceleratedDesc_.format == incoming.format;
            if (!pendingMatches) {
                pendingAcceleratedDesc_ = incoming;
                pendingAccelerated_ = true;
                logger::info("CEF accelerated shared texture description requested {}x{} DXGI format {}.",
                             incoming.width, incoming.height, static_cast<unsigned int>(incoming.format));
            }
            return false;
        }

        renderContext_->CopyResource(texture_.Get(), sharedTexture.Get());
        hasFrame_ = true;
        if (!firstAcceleratedCopyLogged_) {
            logger::info("First accelerated CEF overlay frame copied: {}x{} DXGI format {}.", desc_.width,
                         desc_.height, static_cast<unsigned int>(desc_.format));
            firstAcceleratedCopyLogged_ = true;
        }
        NoteActiveModeChangeLocked(Mode::Accelerated);
        return true;
    }

    bool OverlayTexture::UploadBgra32(const std::byte* pixels, std::uint32_t width, std::uint32_t height,
                                     std::uint32_t srcStride)
    {
        if (!pixels || width == 0 || height == 0) {
            return false;
        }

        std::lock_guard lock(mutex_);
        if (!renderDevice_ || !renderContext_) {
            return false;
        }

        const Desc desc{width, height, DXGI_FORMAT_B8G8R8A8_UNORM};
        if (!texture_ || !srv_ || mode_ != Mode::Cpu || desc_.width != desc.width ||
            desc_.height != desc.height || desc_.format != desc.format) {
            if (!AllocateCpuLocked(desc)) {
                return false;
            }
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = renderContext_->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
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
        renderContext_->Unmap(texture_.Get(), 0);

        hasFrame_ = true;
        NoteActiveModeChangeLocked(Mode::Cpu);
        return true;
    }

    void OverlayTexture::ReleaseResources()
    {
        std::lock_guard lock(mutex_);
        if (texture_ || srv_ || renderDevice_ || renderContext_) {
            logger::info("Releasing CEF overlay D3D resources.");
        }
        srv_.Reset();
        texture_.Reset();
        renderContext_.Reset();
        renderDevice1_.Reset();
        renderDevice_.Reset();
        d3dMultithread_.Reset();
        desc_ = {};
        pendingAcceleratedDesc_ = {};
        mode_ = Mode::None;
        activeMode_ = Mode::None;
        pendingAccelerated_ = false;
        hasFrame_ = false;
        multithreadProtectionConfigured_ = false;
        bridgeUnavailableLogged_ = false;
    }

    ID3D11ShaderResourceView* OverlayTexture::GetSrv() const
    {
        std::lock_guard lock(mutex_);
        return hasFrame_ ? srv_.Get() : nullptr;
    }

    std::uint32_t OverlayTexture::GetWidth() const
    {
        std::lock_guard lock(mutex_);
        return hasFrame_ ? desc_.width : 0;
    }

    std::uint32_t OverlayTexture::GetHeight() const
    {
        std::lock_guard lock(mutex_);
        return hasFrame_ ? desc_.height : 0;
    }

    bool OverlayTexture::HasFrame() const
    {
        std::lock_guard lock(mutex_);
        return hasFrame_;
    }

    OverlayTexture::Mode OverlayTexture::GetActiveMode() const
    {
        std::lock_guard lock(mutex_);
        return activeMode_;
    }
}
