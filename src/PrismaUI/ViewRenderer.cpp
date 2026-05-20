#include "ViewRenderer.h"

#include <DirectXTK/SimpleMath.h>

#include "Cef/Browser/CefRuntime.h"
#include "Core.h"
#include "InputHandler.h"
#include "Utils/D3DStateGuard.h"

namespace PrismaUI::ViewRenderer {
    using namespace Core;

    using PrismaUI::Utils::D3DStateGuard;

    void DrawViews() {
        if (!spriteBatch || !commonStates) return;
        DrawCefOverlay();
    }

    bool DrawCefOverlay() {
        if (!spriteBatch || !commonStates || !d3dContext) return false;

        auto& cefRuntime = Cef::CefRuntime::GetSingleton();
        ID3D11ShaderResourceView* overlaySrv = cefRuntime.GetOverlaySrv();
        const uint32_t overlayWidth = cefRuntime.GetOverlayWidth();
        const uint32_t overlayHeight = cefRuntime.GetOverlayHeight();
        if (!overlaySrv || overlayWidth == 0 || overlayHeight == 0) {
            return false;
        }

        try {
            D3DStateGuard stateGuard(d3dContext);
            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());

            const DirectX::SimpleMath::Vector2 position(0.0f, 0.0f);
            const RECT sourceRect = {0, 0, static_cast<long>(overlayWidth), static_cast<long>(overlayHeight)};
            spriteBatch->Draw(overlaySrv, position, &sourceRect, DirectX::Colors::White, 0.f,
                              DirectX::SimpleMath::Vector2::Zero, 1.0f, DirectX::SpriteEffects_None, 0.f);

            spriteBatch->End();
            return true;
        } catch (const std::exception& e) {
            logger::error("Error drawing CEF overlay texture: {}", e.what());
        } catch (...) {
            logger::error("Unknown error drawing CEF overlay texture.");
        }
        return false;
    }

    void DrawCursor() {
        if (!spriteBatch || !commonStates || !cursorTexture || !d3dContext) {
            return;
        }

        if (!PrismaUI::InputHandler::IsAnyInputCaptureActive()) {
            return;
        }

        auto cursor = RE::MenuCursor::GetSingleton();
        if (!cursor) {
            return;
        }

        try {
            D3DStateGuard stateGuard(d3dContext);
            spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());

            const DirectX::SimpleMath::Vector2 position(cursor->cursorPosX, cursor->cursorPosY);
            spriteBatch->Draw(cursorTexture.Get(), position);

            spriteBatch->End();
        } catch (const std::exception& e) {
            logger::error("Error drawing Prisma cursor texture: {}", e.what());
        } catch (...) {
            logger::error("Unknown error drawing Prisma cursor texture.");
        }
    }
}
