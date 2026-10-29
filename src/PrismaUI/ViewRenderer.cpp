#include "ViewRenderer.h"

#include "Core.h"
#include "InputHandler.h"
#include "Inspector.h"

namespace PrismaUI::ViewRenderer {
    using namespace Core;
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
            CopyBitmapToBuffer(viewData, surface->dirty_bounds());
            surface->ClearDirtyBounds();
        }

        // Render inspector view if visible
        Inspector::RenderInspectorView(viewData);
    }

    // Copies the part of the surface that changed (dirtyBounds) into the view's pixel buffer, and adds it to the
    // buffer's pending dirty rectangle, so the texture update sends only that part to the GPU.
    void CopyBitmapToBuffer(std::shared_ptr<Core::PrismaView> viewData, const IntRect& dirtyBounds) {
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
                const bool sameLayout = viewData->pixelBuffer.size() == required_size &&
                                        viewData->bufferWidth == width && viewData->bufferHeight == height &&
                                        viewData->bufferStride == stride;
                uint32_t left = 0, top = 0, right = width, bottom = height;
                if (sameLayout) {
                    left = static_cast<uint32_t>(std::clamp(dirtyBounds.left, 0, static_cast<int>(width)));
                    top = static_cast<uint32_t>(std::clamp(dirtyBounds.top, 0, static_cast<int>(height)));
                    right = static_cast<uint32_t>(std::clamp(dirtyBounds.right, 0, static_cast<int>(width)));
                    bottom = static_cast<uint32_t>(std::clamp(dirtyBounds.bottom, 0, static_cast<int>(height)));
                    const size_t offset = static_cast<size_t>(left) * 4;
                    const size_t bytes = right > left ? static_cast<size_t>(right - left) * 4 : 0;
                    for (uint32_t y = top; y < bottom && bytes > 0; ++y) {
                        const size_t row = static_cast<size_t>(y) * stride + offset;
                        memcpy(viewData->pixelBuffer.data() + row, static_cast<std::byte*>(pixels) + row, bytes);
                    }
                } else {
                    viewData->pixelBuffer.resize(required_size);
                    memcpy(viewData->pixelBuffer.data(), pixels, required_size);
                    viewData->bufferWidth = width;
                    viewData->bufferHeight = height;
                    viewData->bufferStride = stride;
                    viewData->dirtyRight = 0;  // the whole buffer is new
                }
                if (right > left && bottom > top) {
                    if (viewData->dirtyRight <= viewData->dirtyLeft) {
                        viewData->dirtyLeft = left;
                        viewData->dirtyTop = top;
                        viewData->dirtyRight = right;
                        viewData->dirtyBottom = bottom;
                    } else {
                        viewData->dirtyLeft = std::min(viewData->dirtyLeft, left);
                        viewData->dirtyTop = std::min(viewData->dirtyTop, top);
                        viewData->dirtyRight = std::max(viewData->dirtyRight, right);
                        viewData->dirtyBottom = std::max(viewData->dirtyBottom, bottom);
                    }
                }
                success = viewData->dirtyRight > viewData->dirtyLeft;
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

    void ReleaseViewTexture(Core::PrismaView* viewData) {
        if (!viewData) return;

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
            if (!viewData->pixelBuffer.empty() && viewData->bufferWidth > 0 && viewData->bufferHeight > 0 &&
                viewData->dirtyRight > viewData->dirtyLeft) {
                const D3D11_BOX dirty{viewData->dirtyLeft, viewData->dirtyTop, 0, viewData->dirtyRight,
                                      viewData->dirtyBottom, 1};
                CopyPixelsToTexture(viewData.get(), viewData->pixelBuffer.data(), viewData->bufferWidth,
                                    viewData->bufferHeight, viewData->bufferStride, dirty);
                viewData->dirtyLeft = viewData->dirtyTop = viewData->dirtyRight = viewData->dirtyBottom = 0;
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

    // Sends the dirty box of the pixel buffer to the view's texture (the whole buffer when the texture is new).
    void CopyPixelsToTexture(Core::PrismaView* viewData, void* pixels, uint32_t width, uint32_t height,
                             uint32_t stride, D3D11_BOX dirty) {
        if (!viewData || !d3dDevice || !d3dContext || !pixels || width == 0 || height == 0) return;

        if (!viewData->texture || viewData->textureWidth != width || viewData->textureHeight != height) {
            logger::debug("View [{}]: Creating/Recreating texture ({}x{})", viewData->id, width, height);
            ReleaseViewTexture(viewData);
            D3D11_TEXTURE2D_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;  // updated in parts with UpdateSubresource
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;

            HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &viewData->texture);

            if (FAILED(hr)) {
                logger::critical("View [{}]: Failed to create texture! HR={:#X}", viewData->id, hr);
                ReleaseViewTexture(viewData);
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
                ReleaseViewTexture(viewData);
                return;
            }

            viewData->textureWidth = width;
            viewData->textureHeight = height;
            logger::debug("View [{}]: Texture/SRV created/resized.", viewData->id);
            dirty = D3D11_BOX{0, 0, 0, width, height, 1};  // a new texture holds nothing yet
        }

        const std::byte* source =
            static_cast<const std::byte*>(pixels) + static_cast<size_t>(dirty.top) * stride + dirty.left * 4;
        d3dContext->UpdateSubresource(viewData->texture, 0, &dirty, source, stride, 0);
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

        spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());

        DirectX::SimpleMath::Vector2 position(cursor->cursorPosX, cursor->cursorPosY);
        spriteBatch->Draw(cursorTexture.Get(), position);

        spriteBatch->End();

        d3dContext->OMSetBlendState(backupBlendState, backupBlendFactor, backupSampleMask);
        d3dContext->OMSetDepthStencilState(backupDepthStencilState, backupStencilRef);
        d3dContext->RSSetState(backupRasterizerState);

        if (backupBlendState) backupBlendState->Release();
        if (backupDepthStencilState) backupDepthStencilState->Release();
        if (backupRasterizerState) backupRasterizerState->Release();
    }

    void DrawViews() {
        if (!spriteBatch || !commonStates) return;

        std::vector<std::shared_ptr<Core::PrismaView>> viewsToDraw;
        {
            std::shared_lock lock(viewsMutex);
            viewsToDraw.reserve(views.size());
            for (const auto& pair : views) {
                if (pair.second && !pair.second->isHidden.load() && !pair.second->pendingResourceRelease.load() &&
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

            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());

            for (const auto& viewData : viewsToDraw) {
                DrawSingleTexture(viewData);
            }

            spriteBatch->End();

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

    void DrawSingleTexture(std::shared_ptr<Core::PrismaView> viewData) {
        if (!viewData || !viewData->textureView || viewData->textureWidth == 0 || viewData->textureHeight == 0) return;

        // Draw main view
        DirectX::SimpleMath::Vector2 position(0.0f, 0.0f);
        RECT sourceRect = {0, 0, (long)viewData->textureWidth, (long)viewData->textureHeight};

        spriteBatch->Draw(viewData->textureView, position, &sourceRect, DirectX::Colors::White, 0.f,
                          DirectX::SimpleMath::Vector2::Zero, 1.0f, DirectX::SpriteEffects_None, 0.f);

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
