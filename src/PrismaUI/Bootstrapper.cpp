#include "Bootstrapper.h"

#include "Cef/Browser/CefRuntime.h"
#include "Core.h"
#include "Globals.h"
#include "Hooks/HookInstaller.h"
#include "Hooks/HooksLib.h"
#include "InputHandler.h"
#include "Menus/CursorMenu/CursorMenu.h"
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

    static std::optional<RenderData> TryGetRenderData() {
        auto renderManager = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderManager) {
            logger::critical("RenderManager is null!");
            return std::nullopt;
        }

        auto& runtimeData = renderManager->GetRuntimeData();
        if (!runtimeData.renderWindows || !runtimeData.renderWindows->hWnd) {
            logger::critical("HWND is null!");
            return std::nullopt;
        }

        return RenderData{
            .Hwnd = reinterpret_cast<HWND>(runtimeData.renderWindows->hWnd),
            .ScreenSize = renderManager->GetScreenSize(),
            .D3DDevice = reinterpret_cast<ID3D11Device*>(runtimeData.forwarder),
            .D3DContext = reinterpret_cast<ID3D11DeviceContext*>(runtimeData.context),
        };
    }

    static bool InitializeImpl() {
        logger::info("Initializing PrismaUI...");

        if (!MainThreadScheduler.IsTargetThread()) {
            logger::critical("PrismaUI must be initialized on the main thread");
            return false;
        }

        auto renderDataOpt = TryGetRenderData();
        if (!renderDataOpt) {
            return false;
        }

        auto renderData = renderDataOpt.value();
        if (!Cef::CefRuntime::GetSingleton().Initialize(renderData.Hwnd, renderData.ScreenSize.width,
                                                        renderData.ScreenSize.height, renderData.D3DDevice,
                                                        renderData.D3DContext)) {
            logger::critical("CefRuntime initialization failed");
            return false;
        }

        if (!InputHandler::GetSingleton().Initialize(renderData.Hwnd, &Core::views, &Core::viewsMutex)) {
            logger::critical("InputHandler initialization failed");
            return false;
        }

        if (!Renderer::GetSingleton().Initialize(&Cef::CefRuntime::GetSingleton(), &InputHandler::GetSingleton(),
                                                 renderData.Hwnd, renderData.D3DDevice, renderData.D3DContext)) {
            logger::critical("Renderer initialization failed");
            return false;
        }

        CursorMenuEx::InstallHook();
        Hooks::HookInstaller<Hooks::RendererBegin>::Create()
            .AddPreHandler([](RE::BSGraphics::Renderer*, uint32_t) {
                InputHandler::GetSingleton().ProcessEvents();
                Renderer::GetSingleton().BeginRender();
            })
            .Install();

        Hooks::HookInstaller<Hooks::D3DPresentHook>::Create()
            .AddPreHandler([](uint32_t) { Renderer::GetSingleton().EndRender(); })
            .Install();

        logger::info("PrismaUI successfully initialized");

        return true;
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

    bool Initialize() {
        [[likely]]
        if (IsInitialized) {
            return true;
        }

        if (IsShutdownStarted) {
            return false;
        }

        IsInitialized = InitializeImpl();
        if (IsInitialized) {
            InputHandler::GetSingleton().OnExit([] { MainThreadScheduler.Post([] { Shutdown(); }); });
        } else {
            Shutdown();
        }

        return IsInitialized;
    }
}
