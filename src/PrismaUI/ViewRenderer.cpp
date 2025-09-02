#include "ViewRenderer.h"
#include "Core.h"
#include "InputHandler.h"
#include <DirectXMath.h>

namespace PrismaUI::ViewRenderer {
	using namespace Core;

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
		if (!spriteBatch || !commonStates || !gpuDriver) return;

		std::vector<std::shared_ptr<Core::PrismaView>> viewsToDraw;
		{
			std::shared_lock lock(viewsMutex);
			viewsToDraw.reserve(views.size());
			for (const auto& pair : views) {
				if (pair.second && !pair.second->isHidden.load() && pair.second->ultralightView) {
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

			spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->NonPremultiplied());

			for (const auto& viewData : viewsToDraw) {
                if (!viewData->ultralightView) continue;

                ultralight::RenderTarget target = viewData->ultralightView->render_target();
                if (target.texture_id == 0) {
                    // logger::debug("DrawViews: View [{}] has no texture_id, skipping.", viewData->id);
                    continue;
                }
                
                // logger::debug("DrawViews: Attempting to draw View [{}], texture_id: {}, uv: {{ {}, {}, {}, {} }}", 
                //     viewData->id, target.texture_id, target.uv_coords.left, target.uv_coords.top, target.uv_coords.right, target.uv_coords.bottom);

                ID3D11ShaderResourceView* srv = gpuDriver->GetShaderResourceView(target.texture_id);
                if (!srv) {
                    logger::warn("DrawViews: View [{}], GetShaderResourceView returned null for texture_id: {}", viewData->id, target.texture_id);
                    continue;
                }

                D3D11_TEXTURE2D_DESC desc;
                ID3D11Resource* res = nullptr;
                srv->GetResource(&res);
                if (res) {
                    static_cast<ID3D11Texture2D*>(res)->GetDesc(&desc);
                    res->Release();
                } else {
                    continue;
                }

                RECT sourceRect;
                sourceRect.left = static_cast<LONG>(target.uv_coords.left * desc.Width);
                sourceRect.top = static_cast<LONG>(target.uv_coords.top * desc.Height);
                sourceRect.right = static_cast<LONG>(target.uv_coords.right * desc.Width);
                sourceRect.bottom = static_cast<LONG>(target.uv_coords.bottom * desc.Height);

                spriteBatch->Draw(srv, DirectX::SimpleMath::Vector2(0, 0), &sourceRect);
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
}
