#pragma once

#include <CommonStates.h>
#include <SpriteBatch.h>
#include <wrl/client.h>

#include "Cef/Browser/CefRuntime.h"
#include "InputHandler.h"

namespace PrismaUI {
    class Renderer {
    public:
        static Renderer& GetSingleton();

        bool Initialize(Cef::CefRuntime* cefRuntime, InputHandler* inputHandler, HWND hwnd, ID3D11Device* d3dDevice,
                        ID3D11DeviceContext* d3dContext);

        void BeginRender();

        void EndRender() const;

        void Shutdown();

    private:
        bool InitGraphics(HWND hwnd, ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext);

        Cef::CefRuntime* _cefRuntime{};
        InputHandler* _inputHandler{};
        ID3D11Device* _d3dDevice{};
        ID3D11DeviceContext* _d3dContext{};
        HWND _hWnd{};
        RE::BSGraphics::ScreenSize _screenSize{};
        std::unique_ptr<DirectX::SpriteBatch> _spriteBatch;
        std::unique_ptr<DirectX::CommonStates> _commonStates;
        bool _isInitialized = false;
    };
}
