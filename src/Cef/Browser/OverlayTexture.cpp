#include "Cef/Browser/OverlayTexture.h"

#include <chrono>
#include <cstring>
#include <utility>

#include "PCH.h"

namespace PrismaUI::Cef {
    namespace {
        constexpr std::chrono::milliseconds kAcceleratedCopyWaitTimeout{100};
    }

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

    bool OverlayTexture::SubmitAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle) {
        if (!sharedTextureHandle) {
            return false;
        }

        auto request = std::make_shared<AcceleratedCopyRequest>();
        request->sharedTextureHandle = sharedTextureHandle;

        bool shouldLogTimeout = false;
        {
            std::unique_lock lock(acceleratedCopyMutex_);
            if (pendingAcceleratedCopy_) {
                pendingAcceleratedCopy_->cancelled = true;
                pendingAcceleratedCopy_->completed = true;
                pendingAcceleratedCopy_->copied = false;
            }

            pendingAcceleratedCopy_ = request;
            acceleratedCopyCv_.notify_all();

            const auto deadline = std::chrono::steady_clock::now() + kAcceleratedCopyWaitTimeout;
            while (!request->completed) {
                if (request->started) {
                    acceleratedCopyCv_.wait(lock, [&request]() { return request->completed; });
                    break;
                }

                if (acceleratedCopyCv_.wait_until(lock, deadline,
                                                  [&request]() { return request->completed || request->started; })) {
                    continue;
                }

                if (!request->started) {
                    if (pendingAcceleratedCopy_ == request) {
                        pendingAcceleratedCopy_.reset();
                    }
                    request->cancelled = true;
                    request->completed = true;
                    request->copied = false;
                    if (!acceleratedCopyTimeoutLogged_) {
                        acceleratedCopyTimeoutLogged_ = true;
                        shouldLogTimeout = true;
                    }
                    acceleratedCopyCv_.notify_all();
                    break;
                }
            }
        }

        if (shouldLogTimeout) {
            logger::warn("CEF accelerated paint dropped: render thread did not consume shared texture within {} ms.",
                         kAcceleratedCopyWaitTimeout.count());
        }

        return request->copied;
    }

    bool OverlayTexture::CopyPendingAcceleratedFrame() {
        std::shared_ptr<AcceleratedCopyRequest> request;
        {
            std::lock_guard lock(acceleratedCopyMutex_);
            request = pendingAcceleratedCopy_;
            if (!request || request->cancelled || request->completed) {
                return false;
            }

            request->started = true;
            acceleratedCopyCv_.notify_all();
        }

        bool copied = false;
        {
            std::lock_guard lock(mutex_);
            copied = CopySharedHandleOnRenderThreadLocked(request->sharedTextureHandle);
        }

        {
            std::lock_guard lock(acceleratedCopyMutex_);
            if (pendingAcceleratedCopy_ == request) {
                pendingAcceleratedCopy_.reset();
            }
            request->copied = copied;
            request->completed = true;
            acceleratedCopyCv_.notify_all();
        }

        return copied;
    }

    bool OverlayTexture::CopySharedHandleOnRenderThreadLocked(HANDLE sharedTextureHandle) {
        if (!sharedTextureHandle) {
            return false;
        }

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
            logger::error("CEF accelerated shared texture had invalid description {}x{} format {}.", sharedDesc.Width,
                          sharedDesc.Height, static_cast<unsigned int>(sharedDesc.Format));
            return false;
        }

        const Desc incoming{sharedDesc.Width, sharedDesc.Height, sharedDesc.Format};
        const bool overlayMatches = texture_ && srv_ && mode_ == Mode::Accelerated && desc_.Matches(sharedDesc);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture = texture_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> targetSrv = srv_;
        if (!overlayMatches && !CreateAcceleratedResourcesLocked(incoming, targetTexture, targetSrv)) {
            return false;
        }

        renderContext_->CopyResource(targetTexture.Get(), sharedTexture.Get());
        if (!overlayMatches) {
            texture_ = std::move(targetTexture);
            srv_ = std::move(targetSrv);
            desc_ = incoming;
            mode_ = Mode::Accelerated;
            logger::info("Created accelerated CEF overlay texture {}x{} DXGI format {}.", desc_.width, desc_.height,
                         static_cast<unsigned int>(desc_.format));
        }
        hasFrame_ = true;
        if (!firstAcceleratedCopyLogged_) {
            logger::info("First accelerated CEF overlay frame copied: {}x{} DXGI format {}.", desc_.width, desc_.height,
                         static_cast<unsigned int>(desc_.format));
            firstAcceleratedCopyLogged_ = true;
        }
        NoteActiveModeChangeLocked(Mode::Accelerated);
        return true;
    }

    bool OverlayTexture::UploadBgra32(const std::byte* pixels, std::uint32_t width, std::uint32_t height,
                                      std::uint32_t srcStride) {
        if (!pixels || width == 0 || height == 0) {
            return false;
        }

        std::lock_guard lock(mutex_);
        if (!renderDevice_ || !renderContext_) {
            return false;
        }

        const Desc desc{width, height, DXGI_FORMAT_B8G8R8A8_UNORM};
        const bool overlayMatches = texture_ && srv_ && mode_ == Mode::Cpu && desc_.width == desc.width &&
                                    desc_.height == desc.height && desc_.format == desc.format;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture = texture_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> targetSrv = srv_;
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
            srv_ = std::move(targetSrv);
            desc_ = desc;
            mode_ = Mode::Cpu;
            logger::warn("Created degraded CPU fallback CEF overlay texture {}x{}.", desc_.width, desc_.height);
        }
        hasFrame_ = true;
        NoteActiveModeChangeLocked(Mode::Cpu);
        return true;
    }

    void OverlayTexture::ReleaseResources() {
        {
            std::lock_guard lock(acceleratedCopyMutex_);
            if (pendingAcceleratedCopy_) {
                pendingAcceleratedCopy_->cancelled = true;
                pendingAcceleratedCopy_->completed = true;
                pendingAcceleratedCopy_->copied = false;
                pendingAcceleratedCopy_.reset();
            }
        }
        acceleratedCopyCv_.notify_all();

        std::lock_guard lock(mutex_);
        if (texture_ || srv_ || renderDevice_ || renderContext_) {
            logger::info("Releasing CEF overlay D3D resources.");
        }
        srv_.Reset();
        texture_.Reset();
        renderContext_.Reset();
        renderDevice1_.Reset();
        renderDevice_.Reset();
        desc_ = {};
        mode_ = Mode::None;
        activeMode_ = Mode::None;
        hasFrame_ = false;
        bridgeUnavailableLogged_ = false;
    }

    ID3D11ShaderResourceView* OverlayTexture::GetSrv() const {
        std::lock_guard lock(mutex_);
        return hasFrame_ ? srv_.Get() : nullptr;
    }

    std::uint32_t OverlayTexture::GetWidth() const {
        std::lock_guard lock(mutex_);
        return hasFrame_ ? desc_.width : 0;
    }

    std::uint32_t OverlayTexture::GetHeight() const {
        std::lock_guard lock(mutex_);
        return hasFrame_ ? desc_.height : 0;
    }

    bool OverlayTexture::HasFrame() const {
        std::lock_guard lock(mutex_);
        return hasFrame_;
    }

    OverlayTexture::Mode OverlayTexture::GetActiveMode() const {
        std::lock_guard lock(mutex_);
        return activeMode_;
    }
}
