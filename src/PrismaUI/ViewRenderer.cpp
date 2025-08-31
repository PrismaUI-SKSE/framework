#include "ViewRenderer.h"
#include "Core.h"
#include "InputHandler.h"

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
				if (pair.second && !pair.second->isHidden) {
					viewsToRender.push_back(pair.second);
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
	}

	void CopyBitmapToBuffer(std::shared_ptr<Core::PrismaView> viewData) {
		if (!viewData || !viewData->ultralightView) return;
		BitmapSurface* surface = static_cast<BitmapSurface*>(viewData->ultralightView->surface());
		if (!surface) return;
		RefPtr<Bitmap> bitmap = surface->bitmap();
		if (!bitmap) return;

		void* pixels = bitmap->LockPixels();
		if (!pixels) { logger::error("View [{}]: Failed to lock bitmap pixels.", viewData->id); return; }

		uint32_t width = bitmap->width();
		uint32_t height = bitmap->height();
		uint32_t stride = bitmap->row_bytes();
		size_t required_size = static_cast<size_t>(height) * stride;
		if (width == 0 || height == 0 || required_size == 0) {
			bitmap->UnlockPixels(); return;
		}

		bool success = false;
		{
			std::lock_guard lock(viewData->bufferMutex);
			try {
				if (viewData->pixelBuffer.size() != required_size) { viewData->pixelBuffer.resize(required_size); }
				memcpy(viewData->pixelBuffer.data(), pixels, required_size);
				viewData->bufferWidth = width; viewData->bufferHeight = height; viewData->bufferStride = stride;
				success = true;
			}
			catch (const std::exception& e) {
				logger::error("View [{}]: Exception during pixel buffer copy/resize: {}", viewData->id, e.what());
				viewData->pixelBuffer.clear(); viewData->pixelBuffer.shrink_to_fit();
				viewData->bufferWidth = viewData->bufferHeight = viewData->bufferStride = 0;
			}
		}

		bitmap->UnlockPixels();
		if (success) viewData->newFrameReady = true;
		else viewData->newFrameReady = false;
	}

	void ReleaseViewTexture(Core::PrismaView* viewData) {
		if (!viewData) return;

		if (viewData->textureView) { viewData->textureView->Release(); viewData->textureView = nullptr; }
		if (viewData->texture) { viewData->texture->Release(); viewData->texture = nullptr; }
		viewData->textureWidth = 0; viewData->textureHeight = 0;
	}

	void UpdateSingleTextureFromBuffer(std::shared_ptr<Core::PrismaView> viewData) {
		if (!viewData) return;

		if (viewData->pendingResourceRelease.load()) {
			logger::debug("UpdateSingleTextureFromBuffer: Releasing D3D resources for View [{}] based on pendingResourceRelease flag", viewData->id);

			ReleaseViewTexture(viewData.get());

			viewData->pendingResourceRelease = false;
			return;
		}

		bool expected = true;
		if (!viewData->newFrameReady.compare_exchange_strong(expected, false)) {
			return;
		}

		std::lock_guard lock(viewData->bufferMutex);
		if (viewData->pixelBuffer.empty() || viewData->bufferWidth == 0 || viewData->bufferHeight == 0) {
			return;
		}

		CopyPixelsToTexture(viewData.get(), viewData->pixelBuffer.data(),
			viewData->bufferWidth, viewData->bufferHeight,
			viewData->bufferStride);
	}

	void CopyPixelsToTexture(Core::PrismaView* viewData, void* pixels, uint32_t width, uint32_t height, uint32_t stride) {
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
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &viewData->texture);

			if (FAILED(hr)) {
				logger::critical("View [{}]: Failed to create texture! HR={:#X}", viewData->id, hr);
				ReleaseViewTexture(viewData); return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc; ZeroMemory(&srvDesc, sizeof(srvDesc));

			srvDesc.Format = desc.Format; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;

			hr = d3dDevice->CreateShaderResourceView(viewData->texture, &srvDesc, &viewData->textureView);

			if (FAILED(hr)) {
				logger::critical("View [{}]: Failed to create SRV! HR={:#X}", viewData->id, hr); ReleaseViewTexture(viewData);
				return;
			}

			viewData->textureWidth = width;
			viewData->textureHeight = height;
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
		}
		else {
			for (uint32_t y = 0; y < height; ++y) memcpy(dest + y * destPitch, source + y * stride, stride);
		}

		d3dContext->Unmap(viewData->texture, 0);
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
				if (pair.second && !pair.second->isHidden.load() &&
					!pair.second->pendingResourceRelease.load() &&
					pair.second->textureView) {
					viewsToDraw.push_back(pair.second);
				}
			}
		}

		if (viewsToDraw.empty()) return;

		try {
			ID3D11BlendState* backupBlendState = nullptr; FLOAT backupBlendFactor[4]; UINT backupSampleMask = 0;
			ID3D11DepthStencilState* backupDepthStencilState = nullptr; UINT backupStencilRef = 0;
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

		}
		catch (const std::exception& e) {
			logger::error("Error during SpriteBatch drawing loop: {}", e.what());
		}
		catch (...) {
			logger::error("Unknown error during SpriteBatch drawing loop.");
		}
	}

	void DrawSingleTexture(std::shared_ptr<Core::PrismaView> viewData) {
		if (!viewData || !viewData->textureView || viewData->textureWidth == 0 || viewData->textureHeight == 0) return;

		DirectX::SimpleMath::Vector2 position(0.0f, 0.0f);
		RECT sourceRect = { 0, 0, (long)viewData->textureWidth, (long)viewData->textureHeight };

		spriteBatch->Draw(
			viewData->textureView, position, &sourceRect,
			DirectX::Colors::White, 0.f, DirectX::SimpleMath::Vector2::Zero,
			1.0f, DirectX::SpriteEffects_None, 0.f
		);
	}
}
