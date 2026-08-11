#include "ViewRenderer.h"

#include "Core.h"
#include "InputHandler.h"
#include "Inspector.h"
#include "ModelPreview.h"

namespace PrismaUI::ViewRenderer {
    using namespace Core;

    namespace {
        /// Releases a view's external composite resources while its mutex is held.
        void ReleaseExternalSurfaceLocked(Core::PrismaView* viewData) {
            if (viewData->externalTextureView) {
                viewData->externalTextureView->Release();
                viewData->externalTextureView = nullptr;
            }
            if (viewData->externalRenderTarget) {
                viewData->externalRenderTarget->Release();
                viewData->externalRenderTarget = nullptr;
            }
            if (viewData->externalTexture) {
                viewData->externalTexture->Release();
                viewData->externalTexture = nullptr;
            }
        }

        /// Releases all view textures while the texture mutex is held.
        void ReleaseViewTextureLocked(Core::PrismaView* viewData) {
            ReleaseExternalSurfaceLocked(viewData);
            if (viewData->textureView) {
                viewData->textureView->Release();
                viewData->textureView = nullptr;
            }
            if (viewData->texture) {
                viewData->texture->Release();
                viewData->texture = nullptr;
            }
            viewData->textureWidth = 0;
            viewData->textureHeight = 0;
        }
    }
    void UpdateLogic() {
        if (renderer) {
            renderer->Update();
        }
    }

    void RenderViews() {
        if (!renderer) return;

        std::vector<std::shared_ptr<Core::PrismaView>> viewsToRender;
        {
            std::shared_lock lock(viewsMutex);
            viewsToRender.reserve(views.size());
            for (const auto& pair : views) {
                const auto& viewPtr = pair.second;
                if (!viewPtr) {
                    logger::warn("RenderViews: Found null shared_ptr in views map for ID [{}]", pair.first);
                    continue;
                }
                if (!viewPtr->isHidden.load()) {
                    viewsToRender.push_back(viewPtr);
                }
            }
        }

        for (const auto& viewData : viewsToRender) {
            RenderSingleView(viewData);
        }
    }

    void RenderSingleView(std::shared_ptr<Core::PrismaView> viewData) {
        if (!viewData || !viewData->ultralightView) return;

        Surface* surface_base = viewData->ultralightView->surface();
        if (!surface_base) return;

        BitmapSurface* surface = static_cast<BitmapSurface*>(surface_base);

        if (viewData->isLoadingFinished && !surface->dirty_bounds().IsEmpty()) {
            CopyBitmapToBuffer(viewData);
            surface->ClearDirtyBounds();
        }

        // Render inspector view if visible
        Inspector::RenderInspectorView(viewData);
    }

    void CopyBitmapToBuffer(std::shared_ptr<Core::PrismaView> viewData) {
        if (!viewData || !viewData->ultralightView) return;
        BitmapSurface* surface = static_cast<BitmapSurface*>(viewData->ultralightView->surface());
        if (!surface) return;
        RefPtr<Bitmap> bitmap = surface->bitmap();
        if (!bitmap) return;

        void* pixels = bitmap->LockPixels();
        if (!pixels) {
            logger::error("View [{}]: Failed to lock bitmap pixels.", viewData->id);
            return;
        }

        uint32_t width = bitmap->width();
        uint32_t height = bitmap->height();
        uint32_t stride = bitmap->row_bytes();
        size_t required_size = static_cast<size_t>(height) * stride;
        if (width == 0 || height == 0 || required_size == 0) {
            bitmap->UnlockPixels();
            return;
        }

        bool success = false;
        {
            std::lock_guard lock(viewData->bufferMutex);
            try {
                if (viewData->pixelBuffer.size() != required_size) {
                    viewData->pixelBuffer.resize(required_size);
                }
                memcpy(viewData->pixelBuffer.data(), pixels, required_size);
                viewData->bufferWidth = width;
                viewData->bufferHeight = height;
                viewData->bufferStride = stride;
                success = true;
            } catch (const std::exception& e) {
                logger::error("View [{}]: Exception during pixel buffer copy/resize: {}", viewData->id, e.what());
                viewData->pixelBuffer.clear();
                viewData->pixelBuffer.shrink_to_fit();
                viewData->bufferWidth = viewData->bufferHeight = viewData->bufferStride = 0;
            }
        }

        bitmap->UnlockPixels();
        if (success)
            viewData->newFrameReady = true;
        else
            viewData->newFrameReady = false;
    }

    /// Releases all D3D11 resources associated with a view.
    void ReleaseViewTexture(Core::PrismaView* viewData) {
        if (!viewData) return;
        std::scoped_lock lock(viewData->textureMutex);
        ReleaseViewTextureLocked(viewData);
    }

    /// Releases the composite surface used by an external host.
    void ReleaseExternalSurface(Core::PrismaView* viewData) {
        if (!viewData) return;
        std::scoped_lock lock(viewData->textureMutex);
        ReleaseExternalSurfaceLocked(viewData);
        viewData->textureGeneration.fetch_add(1);
    }

    void UpdateSingleTextureFromBuffer(std::shared_ptr<Core::PrismaView> viewData) {
        if (!viewData) return;

        if (viewData->pendingResourceRelease.load()) {
            logger::debug(
                "UpdateSingleTextureFromBuffer: Releasing D3D resources for View [{}] based on pendingResourceRelease "
                "flag",
                viewData->id);

            ReleaseViewTexture(viewData.get());
            Inspector::ReleaseInspectorTexture(viewData.get());

            viewData->pendingResourceRelease = false;
            return;
        }

        // Update main view texture if frame is ready
        const bool mainFrameReady = viewData->newFrameReady.exchange(false);
        if (mainFrameReady) {
            std::lock_guard lock(viewData->bufferMutex);
            if (!viewData->pixelBuffer.empty() && viewData->bufferWidth > 0 && viewData->bufferHeight > 0) {
                CopyPixelsToTexture(viewData.get(), viewData->pixelBuffer.data(), viewData->bufferWidth,
                                    viewData->bufferHeight, viewData->bufferStride);
            }
        }

        // Update inspector texture independently (don't gate on main view frame)
        if (viewData->inspectorVisible.load() && viewData->inspectorFrameReady.exchange(false)) {
            std::lock_guard inspectorLock(viewData->inspectorBufferMutex);
            if (!viewData->inspectorPixelBuffer.empty() && viewData->inspectorBufferWidth > 0 &&
                viewData->inspectorBufferHeight > 0) {
                Inspector::CopyInspectorPixelsToTexture(viewData.get(), viewData->inspectorPixelBuffer.data(),
                                                        viewData->inspectorBufferWidth, viewData->inspectorBufferHeight,
                                                        viewData->inspectorBufferStride);
            }
        }
    }

    /// Uploads a BGRA frame and recreates synchronized textures when dimensions change.
    void CopyPixelsToTexture(Core::PrismaView* viewData, void* pixels, uint32_t width, uint32_t height,
                             uint32_t stride) {
        if (!viewData || !d3dDevice || !d3dContext || !pixels || width == 0 || height == 0) return;

        std::scoped_lock textureLock(viewData->textureMutex);

        if (!viewData->texture || viewData->textureWidth != width || viewData->textureHeight != height) {
            logger::debug("View [{}]: Creating/Recreating texture ({}x{})", viewData->id, width, height);
            ReleaseViewTextureLocked(viewData);
            D3D11_TEXTURE2D_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &viewData->texture);

            if (FAILED(hr)) {
                logger::critical("View [{}]: Failed to create texture! HR={:#X}", viewData->id, hr);
                ReleaseViewTextureLocked(viewData);
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
            ZeroMemory(&srvDesc, sizeof(srvDesc));

            srvDesc.Format = desc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;

            hr = d3dDevice->CreateShaderResourceView(viewData->texture, &srvDesc, &viewData->textureView);

            if (FAILED(hr)) {
                logger::critical("View [{}]: Failed to create SRV! HR={:#X}", viewData->id, hr);
                ReleaseViewTextureLocked(viewData);
                return;
            }

            viewData->textureWidth = width;
            viewData->textureHeight = height;
            viewData->textureGeneration.fetch_add(1);
            logger::debug("View [{}]: Texture/SRV created/resized.", viewData->id);
        }

        D3D11_MAPPED_SUBRESOURCE mappedResource;
        HRESULT hr = d3dContext->Map(viewData->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (FAILED(hr)) {
            logger::error("View [{}]: Failed to map texture! HR={:#X}", viewData->id, hr);
            return;
        }

        std::byte* source = static_cast<std::byte*>(pixels);
        std::byte* dest = static_cast<std::byte*>(mappedResource.pData);
        uint32_t destPitch = mappedResource.RowPitch;

        if (destPitch == stride) {
            memcpy(dest, source, (size_t)height * stride);
        } else {
            for (uint32_t y = 0; y < height; ++y) memcpy(dest + y * destPitch, source + y * stride, stride);
        }

        d3dContext->Unmap(viewData->texture, 0);
    }

    // Once a SpriteBatch queue is poisoned (a flushed sprite faults in the driver),
    // End() throws and leaves the batch stuck "open" -- every later Begin() then
    // nest-throws -> CTD spiral. There is no public "abort"/"reset", so recovery
    // DISCARDS the batch and builds a fresh one (clean flag, empty queue). The
    // destructor never flushes, so dropping a poisoned batch is safe.
    static void RecreateSpriteBatch() {
        try { spriteBatch.reset(); } catch (...) {}
        if (!d3dContext) return;
        try {
            spriteBatch = std::make_unique<DirectX::SpriteBatch>(d3dContext);
        } catch (...) {
            spriteBatch.reset();
            logger::error("Failed to recreate SpriteBatch.");
        }
    }

    // Begin the shared batch; returns false if no usable batch exists this frame.
    static bool BeginSpriteBatchSafe() {
        if (!spriteBatch) RecreateSpriteBatch();
        if (!spriteBatch || !commonStates) return false;
        try {
            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());
            return true;
        } catch (...) {
            logger::warn("SpriteBatch was poisoned on Begin; discarding and recreating it.");
            RecreateSpriteBatch();
            if (!spriteBatch) return false;
            try {
                spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());
                return true;
            } catch (...) {
                logger::error("SpriteBatch Begin failed even after recreate; skipping frame.");
                spriteBatch.reset();  // force a clean rebuild next frame
                return false;
            }
        }
    }

    // End the batch; if the flush faults (poisoned queue) discard the batch so the
    // next frame starts clean instead of inheriting a stuck-open one.
    static void EndSpriteBatchSafe() {
        if (!spriteBatch) return;
        try {
            spriteBatch->End();
        } catch (...) {
            logger::warn("SpriteBatch End faulted; discarding the batch.");
            RecreateSpriteBatch();
        }
    }

    void DrawCursor() {
        if (!spriteBatch || !commonStates || !cursorTexture) {
            return;
        }

        if (!PrismaUI::InputHandler::IsAnyInputCaptureActive()) {
            return;
        }

        auto cursor = RE::MenuCursor::GetSingleton();
        if (!cursor) {
            return;
        }

        ID3D11BlendState* backupBlendState = nullptr;
        FLOAT backupBlendFactor[4];
        UINT backupSampleMask = 0;
        ID3D11DepthStencilState* backupDepthStencilState = nullptr;
        UINT backupStencilRef = 0;
        ID3D11RasterizerState* backupRasterizerState = nullptr;

        d3dContext->OMGetBlendState(&backupBlendState, backupBlendFactor, &backupSampleMask);
        d3dContext->OMGetDepthStencilState(&backupDepthStencilState, &backupStencilRef);
        d3dContext->RSGetState(&backupRasterizerState);

        if (BeginSpriteBatchSafe()) {
            try {
                DirectX::SimpleMath::Vector2 position(cursor->cursorPosX, cursor->cursorPosY);
                spriteBatch->Draw(cursorTexture.Get(), position);
            } catch (const std::exception& e) {
                logger::error("DrawCursor draw failed: {}", e.what());
            } catch (...) {
                logger::error("DrawCursor draw failed: unknown error");
            }
            EndSpriteBatchSafe();
        }

        d3dContext->OMSetBlendState(backupBlendState, backupBlendFactor, backupSampleMask);
        d3dContext->OMSetDepthStencilState(backupDepthStencilState, backupStencilRef);
        d3dContext->RSSetState(backupRasterizerState);

        if (backupBlendState) backupBlendState->Release();
        if (backupDepthStencilState) backupDepthStencilState->Release();
        if (backupRasterizerState) backupRasterizerState->Release();
    }

    /// Draws only views whose presentation remains owned by PrismaUI.
    void DrawViews() {
        if (!spriteBatch || !commonStates) return;

        std::vector<std::shared_ptr<Core::PrismaView>> viewsToDraw;
        {
            std::shared_lock lock(viewsMutex);
            viewsToDraw.reserve(views.size());
            for (const auto& pair : views) {
                if (pair.second && !pair.second->isHidden.load() && !pair.second->externalSurfaceHost.load() &&
                    !pair.second->pendingResourceRelease.load() &&
                    pair.second->textureView) {
                    viewsToDraw.push_back(pair.second);
                }
            }
        }

        if (viewsToDraw.empty()) return;

        std::sort(viewsToDraw.begin(), viewsToDraw.end(),
                  [](const std::shared_ptr<Core::PrismaView>& a, const std::shared_ptr<Core::PrismaView>& b) {
                      return a->order < b->order;
                  });

        try {
            ID3D11BlendState* backupBlendState = nullptr;
            FLOAT backupBlendFactor[4];
            UINT backupSampleMask = 0;
            ID3D11DepthStencilState* backupDepthStencilState = nullptr;
            UINT backupStencilRef = 0;
            ID3D11RasterizerState* backupRasterizerState = nullptr;
            d3dContext->OMGetBlendState(&backupBlendState, backupBlendFactor, &backupSampleMask);
            d3dContext->OMGetDepthStencilState(&backupDepthStencilState, &backupStencilRef);
            d3dContext->RSGetState(&backupRasterizerState);

            if (BeginSpriteBatchSafe()) {
                // Guard each view's draw individually so one bad draw can never skip End().
                // A skipped End() leaves the batch open, so the next Begin() throws
                // "Cannot nest Begin calls" -> uncaught -> CTD (the AddItem-search crash).
                for (const auto& viewData : viewsToDraw) {
                    try {
                        DrawSingleTexture(viewData);
                    } catch (const std::exception& e) {
                        logger::error("DrawSingleTexture failed (view skipped): {}", e.what());
                    } catch (...) {
                        logger::error("DrawSingleTexture failed (view skipped): unknown error");
                    }
                }

                EndSpriteBatchSafe();
            }

            d3dContext->OMSetBlendState(backupBlendState, backupBlendFactor, backupSampleMask);
            d3dContext->OMSetDepthStencilState(backupDepthStencilState, backupStencilRef);
            d3dContext->RSSetState(backupRasterizerState);
            if (backupBlendState) backupBlendState->Release();
            if (backupDepthStencilState) backupDepthStencilState->Release();
            if (backupRasterizerState) backupRasterizerState->Release();

        } catch (const std::exception& e) {
            logger::error("Error during SpriteBatch drawing loop: {}", e.what());
        } catch (...) {
            logger::error("Unknown error during SpriteBatch drawing loop.");
        }
    }

    /// Composites hosted HTML views and model previews into shareable textures.
    void ComposeExternalSurfaces() {
        if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates) return;

        std::vector<std::shared_ptr<Core::PrismaView>> hostedViews;
        {
            std::shared_lock lock(viewsMutex);
            for (const auto& [id, viewData] : views) {
                if (viewData && viewData->externalSurfaceHost.load() &&
                    !viewData->isHidden.load() && !viewData->pendingResourceRelease.load()) {
                    hostedViews.push_back(viewData);
                }
            }
        }

        for (const auto& viewData : hostedViews) {
            std::vector<ModelPreview::FlatDraw> previews;
            ModelPreview::GetFlatOverlays(viewData->id, previews);

            std::scoped_lock textureLock(viewData->textureMutex);
            if (!viewData->texture || !viewData->textureView ||
                viewData->textureWidth == 0 || viewData->textureHeight == 0) continue;

            if (!viewData->externalTexture) {
                D3D11_TEXTURE2D_DESC descriptor{};
                descriptor.Width = viewData->textureWidth;
                descriptor.Height = viewData->textureHeight;
                descriptor.MipLevels = 1;
                descriptor.ArraySize = 1;
                descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                descriptor.SampleDesc.Count = 1;
                descriptor.Usage = D3D11_USAGE_DEFAULT;
                descriptor.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(d3dDevice->CreateTexture2D(
                        &descriptor, nullptr, &viewData->externalTexture)) ||
                    FAILED(d3dDevice->CreateRenderTargetView(
                        viewData->externalTexture, nullptr, &viewData->externalRenderTarget)) ||
                    FAILED(d3dDevice->CreateShaderResourceView(
                        viewData->externalTexture, nullptr, &viewData->externalTextureView))) {
                    logger::error("View [{}]: failed to create external composite surface.", viewData->id);
                    ReleaseExternalSurfaceLocked(viewData.get());
                    continue;
                }
                viewData->textureGeneration.fetch_add(1);
            }

            d3dContext->CopyResource(viewData->externalTexture, viewData->texture);
            if (previews.empty()) continue;

            ID3D11RenderTargetView* previousRenderTarget = nullptr;
            ID3D11DepthStencilView* previousDepthStencil = nullptr;
            d3dContext->OMGetRenderTargets(1, &previousRenderTarget, &previousDepthStencil);
            UINT viewportCount = 1;
            D3D11_VIEWPORT previousViewport{};
            d3dContext->RSGetViewports(&viewportCount, &previousViewport);

            d3dContext->OMSetRenderTargets(1, &viewData->externalRenderTarget, nullptr);
            const D3D11_VIEWPORT viewport{
                0.0f,
                0.0f,
                static_cast<float>(viewData->textureWidth),
                static_cast<float>(viewData->textureHeight),
                0.0f,
                1.0f
            };
            d3dContext->RSSetViewports(1, &viewport);
            if (BeginSpriteBatchSafe()) {
                for (const auto& preview : previews) {
                    if (preview.srv) spriteBatch->Draw(preview.srv, preview.dest);
                }
                EndSpriteBatchSafe();
            }

            d3dContext->OMSetRenderTargets(1, &previousRenderTarget, previousDepthStencil);
            if (viewportCount) d3dContext->RSSetViewports(1, &previousViewport);
            if (previousRenderTarget) previousRenderTarget->Release();
            if (previousDepthStencil) previousDepthStencil->Release();
        }
    }

    void DrawSingleTexture(std::shared_ptr<Core::PrismaView> viewData) {
        if (!viewData || !viewData->textureView || viewData->textureWidth == 0 || viewData->textureHeight == 0) return;

        // Draw main view
        DirectX::SimpleMath::Vector2 position(0.0f, 0.0f);
        RECT sourceRect = {0, 0, (long)viewData->textureWidth, (long)viewData->textureHeight};

        spriteBatch->Draw(viewData->textureView, position, &sourceRect, DirectX::Colors::White, 0.f,
                          DirectX::SimpleMath::Vector2::Zero, 1.0f, DirectX::SpriteEffects_None, 0.f);

        // 3D model preview sprites ride the same batch, over this view's placeholder rects
        std::vector<ModelPreview::FlatDraw> previews;
        ModelPreview::GetFlatOverlays(viewData->id, previews);
        for (const auto& p : previews) {
            if (p.srv) spriteBatch->Draw(p.srv, p.dest);
        }

        // Draw inspector overlay if visible
        if (viewData->inspectorVisible.load() && viewData->inspectorTextureView &&
            viewData->inspectorTextureWidth > 0 && viewData->inspectorTextureHeight > 0) {
            DirectX::SimpleMath::Vector2 inspectorPos(viewData->inspectorPosX, viewData->inspectorPosY);
            // Source rect should use actual texture dimensions
            RECT inspectorSourceRect = {0, 0, (long)viewData->inspectorTextureWidth,
                                        (long)viewData->inspectorTextureHeight};

            spriteBatch->Draw(viewData->inspectorTextureView, inspectorPos, &inspectorSourceRect,
                              DirectX::Colors::White, 0.f, DirectX::SimpleMath::Vector2::Zero, 1.0f,
                              DirectX::SpriteEffects_None, 0.f);
        }
    }
}
