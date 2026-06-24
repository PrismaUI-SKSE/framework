#include "Renderer.h"

#include <CommonStates.h>
#include <SpriteBatch.h>
#include <WICTextureLoader.h>

#include "Hooks/Hooks.h"
#include "Hooks/HooksLib.h"
#include "InputHandler.h"
#include "Menus/FocusMenu/FocusMenu.h"
#include "Utils/D3DStateGuard.h"
#include "Utils/DllLoader.h"
#include "ViewOperationQueue.h"

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
            logger::info("DirectXTK SpriteBatch and CommonStates initialized");
        } catch (const std::exception& e) {
            logger::critical("Failed to initialize DirectXTK: {}", e.what());
            _commonStates.reset();
            _spriteBatch.reset();
            return false;
        }

        auto cursorPath = Utils::GetBasePath() / "misc" / "cursor.png";
        HRESULT hr =
            DirectX::CreateWICTextureFromFile(_d3dDevice, cursorPath.wstring().c_str(), nullptr, &_cursorTexture);
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

    static void RenderCefOverlay(const std::unique_ptr<DirectX::SpriteBatch>& spriteBatch,
                                 const Cef::CefRuntime& cefRuntime) {
        cefRuntime.UpdateOverlayTexture();
        auto overlayInfoOpt = cefRuntime.GetOverlayInfo();
        if (!overlayInfoOpt.has_value()) {
            return;
        }

        auto overlayInfo = overlayInfoOpt.value();
        constexpr Vector2 position(0.0f, 0.0f);
        const RECT sourceRect = {0, 0, static_cast<long>(overlayInfo.Width), static_cast<long>(overlayInfo.Height)};
        spriteBatch->Draw(overlayInfo.Srv, position, &sourceRect, DirectX::Colors::White, 0.f, Vector2::Zero, 1.0f,
                          DirectX::SpriteEffects_None, 0.f);
    }

    void RenderCursor(const std::unique_ptr<DirectX::SpriteBatch>& spriteBatch,
                      const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& cursorTexture,
                      const InputHandler& inputHandler) {
        if (!inputHandler.IsAnyInputCaptureActive()) {
            return;
        }

        auto cursor = RE::MenuCursor::GetSingleton();
        if (!cursor) {
            return;
        }

        const Vector2 position(cursor->cursorPosX, cursor->cursorPosY);
        spriteBatch->Draw(cursorTexture.Get(), position);
    }

    void Renderer::BeginRender() {
        [[unlikely]]
        if (!_isInitialized) {
            return;
        }

        if (auto* renderManager = RE::BSGraphics::Renderer::GetSingleton()) {
            const auto currentScreenSize = renderManager->GetScreenSize();
            if (currentScreenSize.width != 0 && currentScreenSize.height != 0 &&
                (currentScreenSize.width != _screenSize.width || currentScreenSize.height != _screenSize.height)) {
                _screenSize = currentScreenSize;
                _cefRuntime->Resize(_screenSize.width, _screenSize.height);
            }
        }

        ViewOperationQueue::ProcessAllViewOperations();

        _cefRuntime->UpdateOverlayTexture();
        _cefRuntime->BeginFrame();
    }

    void Renderer::EndRender() const {
        [[unlikely]]
        if (!_isInitialized) {
            return;
        }

        Utils::D3DStateGuard stateGuard(_d3dContext);
        _spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, _commonStates->AlphaBlend());

        RenderCefOverlay(_spriteBatch, *_cefRuntime);
        RenderCursor(_spriteBatch, _cursorTexture, *_inputHandler);

        _spriteBatch->End();
    }

    bool Renderer::Initialize(Cef::CefRuntime* cefRuntime, InputHandler* inputHandler) {
        logger::info("Initialization...");
        if (!InitGraphics()) {
            return false;
        }

        auto ui = RE::UI::GetSingleton();
        ui->Register(FocusMenu::MENU_NAME, FocusMenu::Creator);

        _cefRuntime = cefRuntime;
        _cefRuntime->InitOverlayTexture(_d3dDevice, _d3dContext);
        _inputHandler = inputHandler;
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