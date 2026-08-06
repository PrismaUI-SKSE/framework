#include "Bootstrapper.h"

#include "Cef/Browser/CefRuntime.h"
#include "Core.h"
#include "Globals.h"
#include "Hooks/HookInstaller.h"
#include "Hooks/HooksLib.h"
#include "InputHandler.h"
#include "Renderer.h"
#include "ViewManager.h"

namespace PrismaUI::Bootstrapper {
    static inline bool IsInitialized = false;
    static inline std::atomic_bool IsShutdownStarted = false;

    struct RenderData {
        HWND Hwnd;
        RE::BSGraphics::ScreenSize ScreenSize;
        ID3D11Device* D3DDevice;
        ID3D11DeviceContext* D3DContext;
    };

    static std::expected<RenderData, std::string> TryGetRenderData() {
        auto renderManager = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderManager) {
            logger::critical("RenderManager is null");
            return std::unexpected("RenderManager is null");
        }

        auto& runtimeData = renderManager->GetRuntimeData();
        if (!runtimeData.renderWindows || !runtimeData.renderWindows->hWnd) {
            logger::critical("HWND is null");
            return std::unexpected("HWND is null");
        }

        return RenderData{
            .Hwnd = reinterpret_cast<HWND>(runtimeData.renderWindows->hWnd),
            .ScreenSize = renderManager->GetScreenSize(),
            .D3DDevice = reinterpret_cast<ID3D11Device*>(runtimeData.forwarder),
            .D3DContext = reinterpret_cast<ID3D11DeviceContext*>(runtimeData.context),
        };
    }

    static std::expected<void, std::string> InitializeImpl() {
        logger::info("Initializing PrismaUI...");

        if (!MainThreadScheduler.IsTargetThread()) {
            logger::critical("PrismaUI must be initialized on the main thread");
            return std::unexpected("PrismaUI must be initialized on the main thread");
        }

        auto renderDataResult = TryGetRenderData();
        if (!renderDataResult.has_value()) {
            return std::unexpected(renderDataResult.error());
        }

        auto renderData = renderDataResult.value();
        if (!Cef::CefRuntime::GetSingleton().Initialize(renderData.Hwnd, renderData.ScreenSize.width,
                                                        renderData.ScreenSize.height, renderData.D3DDevice,
                                                        renderData.D3DContext)) {
            logger::critical("CefRuntime initialization failed");
            return std::unexpected("CefRuntime initialization failed");
        }

        if (!InputHandler::GetSingleton().Initialize(renderData.Hwnd, &Core::views, &Core::viewsMutex)) {
            logger::critical("InputHandler initialization failed");
            return std::unexpected("InputHandler initialization failed");
        }

        if (!Renderer::GetSingleton().Initialize(&Cef::CefRuntime::GetSingleton(), &InputHandler::GetSingleton(),
                                                 renderData.Hwnd, renderData.D3DDevice, renderData.D3DContext)) {
            logger::critical("Renderer initialization failed");
            return std::unexpected("Renderer initialization failed");
        }

        Hooks::HookInstaller<Hooks::RendererBegin>::Install(
            [](const auto& originalFunc, RE::BSGraphics::Renderer* renderer, uint32_t a) {
                InputHandler::GetSingleton().ProcessEvents();
                Renderer::GetSingleton().BeginRender();
                originalFunc(renderer, a);
            });

        logger::info("PrismaUI successfully initialized");

        return {};
    }

    static void Shutdown() {
        auto expected = false;
        if (!IsShutdownStarted.compare_exchange_strong(expected, true)) {
            return;
        }

        logger::info("Shutdown...");
        ViewManager::Shutdown();
        Renderer::GetSingleton().Shutdown();
        InputHandler::GetSingleton().Shutdown();
        Cef::CefRuntime::GetSingleton().Shutdown();
        logger::info("Shutdown complete");
    }

    std::expected<void, std::string> Initialize() {
        [[likely]]
        if (IsInitialized) {
            return {};
        }

        if (IsShutdownStarted) {
            return std::unexpected("PrismaUI is already shutting down");
        }

        auto initializeResult = InitializeImpl();
        IsInitialized = initializeResult.has_value();
        if (IsInitialized) {
            InputHandler::GetSingleton().OnExit([] { MainThreadScheduler.Post([] { Shutdown(); }); });
        } else {
            Shutdown();
        }

        return initializeResult;
    }
}
