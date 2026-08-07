#include "Renderer.h"

#include <CommonStates.h>
#include <SpriteBatch.h>
#include <WICTextureLoader.h>

#include "InputHandler.h"
#include "Menus/PrismaUIMenu.h"
#include "Menus/Utils.h"
#include "Utils/D3DStateGuard.h"
#include "Utils/DllLoader.h"
#include "ViewOperationQueue.h"

namespace PrismaUI {
    Renderer& Renderer::GetSingleton() {
        static Renderer instance;
        return instance;
    }

    bool Renderer::InitGraphics(HWND hwnd, ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext) {
        _d3dDevice = d3dDevice;
        _d3dContext = d3dContext;
        _hWnd = hwnd;
        _screenSize = RE::BSGraphics::Renderer::GetSingleton()->GetScreenSize();

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

        _spriteBatch->End();
    }

    bool Renderer::Initialize(Cef::CefRuntime* cefRuntime, InputHandler* inputHandler, HWND hwnd,
                              ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext) {
        logger::info("Initialization...");
        if (!InitGraphics(hwnd, d3dDevice, d3dContext)) {
            return false;
        }

        auto ui = RE::UI::GetSingleton();
        ui->Register(Menus::PrismaUIMenu::MENU_NAME, Menus::PrismaUIMenu::Creator);
        Menus::ShowMenu(Menus::PrismaUIMenu::MENU_NAME);

        _cefRuntime = cefRuntime;
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