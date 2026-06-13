#include "Renderer.h"

#include <CommonStates.h>
#include <SpriteBatch.h>
#include <WICTextureLoader.h>

#include "InputHandler.h"
#include "ViewOperationQueue.h"
#include "Hooks/Hooks.h"
#include "Menus/FocusMenu/FocusMenu.h"
#include "Utils/D3DStateGuard.h"
#include "Utils/DllLoader.h"

namespace PrismaUI {
    Renderer& Renderer::GetSingleton() {
        static Renderer instance;
        return instance;
    }

    bool Renderer::InitGraphics() {
        auto* renderManager = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderManager) {
            logger::critical("RenderManager is null!");
            return false;
        }

        auto runtimeData = renderManager->GetRuntimeData();
        _d3dDevice = reinterpret_cast<ID3D11Device*>(runtimeData.forwarder);
        _d3dContext = reinterpret_cast<ID3D11DeviceContext*>(runtimeData.context);
        if (!_d3dDevice || !_d3dContext) {
            logger::critical("D3D device or context is null!");
            return false;
        }

        if (!runtimeData.renderWindows || !runtimeData.renderWindows->hWnd) {
            logger::critical("HWND is null!");
            return false;
        }

        _hWnd = reinterpret_cast<HWND>(runtimeData.renderWindows->hWnd);
        _screenSize = renderManager->GetScreenSize();

        try {
            _commonStates = std::make_unique<DirectX::CommonStates>(_d3dDevice);
            _spriteBatch = std::make_unique<DirectX::SpriteBatch>(_d3dContext);
            logger::info("DirectXTK SpriteBatch and CommonStates (re)initialized.");
        } catch (const std::exception& e) {
            logger::critical("Failed to initialize DirectXTK: {}", e.what());
            _commonStates.reset();
            _spriteBatch.reset();
            return false;
        }

        auto cursorPath = Utils::GetBasePath() / "misc" / "cursor.png";
        HRESULT hr = DirectX::CreateWICTextureFromFile(_d3dDevice, cursorPath.wstring().c_str(), nullptr, &_cursorTexture);
        if (SUCCEEDED(hr)) {
            logger::info("Cursor texture loaded successfully.");
        } else {
            logger::error("Failed to load cursor texture from '{}'. HRESULT: 0x{:08X}", cursorPath.string(),
                          static_cast<unsigned int>(hr));
            _cursorTexture.Reset();
            return false;
        }

        return true;
    }

    static void RenderCefOverlay(const std::unique_ptr<DirectX::SpriteBatch>& spriteBatch, const Cef::CefRuntime& cefRuntime) {
        ID3D11ShaderResourceView* overlaySrv = cefRuntime.GetOverlaySrv();
        const uint32_t overlayWidth = cefRuntime.GetOverlayWidth();
        const uint32_t overlayHeight = cefRuntime.GetOverlayHeight();
        if (!overlaySrv || overlayWidth == 0 || overlayHeight == 0) {
            return;
        }

        constexpr Vector2 position(0.0f, 0.0f);
        const RECT sourceRect = {0, 0, static_cast<long>(overlayWidth), static_cast<long>(overlayHeight)};
        spriteBatch->Draw(overlaySrv, position, &sourceRect, DirectX::Colors::White, 0.f,
                          Vector2::Zero, 1.0f, DirectX::SpriteEffects_None, 0.f);
    }

    void RenderCursor(const std::unique_ptr<DirectX::SpriteBatch>& spriteBatch, const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& cursorTexture) {
        if (!InputHandler::IsAnyInputCaptureActive()) {
            return;
        }

        auto cursor = RE::MenuCursor::GetSingleton();
        if (!cursor) {
            return;
        }

        const Vector2 position(cursor->cursorPosX, cursor->cursorPosY);
        spriteBatch->Draw(cursorTexture.Get(), position);
    }

    void Renderer::Render() const {
        Utils::D3DStateGuard stateGuard(_d3dContext);
        _spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, _commonStates->AlphaBlend());

        RenderCefOverlay(_spriteBatch, *_cefRuntime);
        RenderCursor(_spriteBatch, _cursorTexture);

        _spriteBatch->End();
    }

    inline REL::Relocation<Hooks::D3DPresentHook::D3DPresentFunc> RealD3dPresentFunc;

    void Renderer::D3DPresent(uint32_t a_p1) {
        RealD3dPresentFunc(a_p1);

        auto& self = GetSingleton();
        if (auto* renderManager = RE::BSGraphics::Renderer::GetSingleton()) {
            const auto currentScreenSize = renderManager->GetScreenSize();
            if (currentScreenSize.width != 0 && currentScreenSize.height != 0 &&
                (currentScreenSize.width != self._screenSize.width || currentScreenSize.height != self._screenSize.height)) {
                self._screenSize = currentScreenSize;
                Cef::CefRuntime::GetSingleton().Resize(self._screenSize.width, self._screenSize.height);
            }
        }

        self._cefRuntime->BeginFrame();
        self._cefRuntime->UpdateOverlayTexture(self._d3dDevice, self._d3dContext);

        // Process pending operations and queued input for all views.
        ViewOperationQueue::ProcessAllViewOperations();
        InputHandler::ProcessEvents();

        self.Render();
    }

    bool Renderer::Initialize(Cef::CefRuntime* cefRuntime) {
        logger::info("Initialization...");
        if (!InitGraphics()) {
            return false;
        }

        auto ui = RE::UI::GetSingleton();
        ui->Register(FocusMenu::MENU_NAME, FocusMenu::Creator);

        RealD3dPresentFunc = Hooks::D3DPresentHook::Install(&D3DPresent);

        _cefRuntime = cefRuntime;
        _isInitialized = true;

        logger::info("Initialized");
        return true;
    }

    void Renderer::Shutdown() {
        if (!_isInitialized) {
            return;
        }

        logger::info("Shutdown...");

        _cursorTexture.Reset();
        _spriteBatch.reset();
        _commonStates.reset();

        _cefRuntime = nullptr;
        _d3dDevice = nullptr;
        _d3dContext = nullptr;
        _hWnd = nullptr;
        _isInitialized = false;

        logger::info("Shutdown complete");
    }
}