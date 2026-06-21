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

    static std::optional<std::tuple<HWND, RE::BSGraphics::ScreenSize>> TryGetRenderWindow() {
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

        return std::make_tuple(reinterpret_cast<HWND>(runtimeData.renderWindows->hWnd), renderManager->GetScreenSize());
    }

    static bool InitializeImpl() {
        logger::info("Initializing PrismaUI...");

        if (!MainThreadScheduler.IsTargetThread()) {
            logger::critical("PrismaUI must be initialized on the main thread");
            return false;
        }

        auto renderWindowOpt = TryGetRenderWindow();
        if (!renderWindowOpt) {
            return false;
        }

        auto [hWnd, screenSize] = renderWindowOpt.value();
        if (!Cef::CefRuntime::GetSingleton().Initialize(hWnd, screenSize.width, screenSize.height)) {
            logger::critical("CefRuntime initialization failed");
            return false;
        }

        if (!InputHandler::GetSingleton().Initialize(hWnd, &Core::views, &Core::viewsMutex)) {
            logger::critical("InputHandler initialization failed");
            return false;
        }

        if (!Renderer::GetSingleton().Initialize(&Cef::CefRuntime::GetSingleton(), &InputHandler::GetSingleton())) {
            logger::critical("Renderer initialization failed");
            return false;
        }

        CursorMenuEx::InstallHook();
        Hooks::HookInstaller<Hooks::RendererBegin>::Create()
            .AddPreHandler([](RE::BSGraphics::Renderer*, uint32_t) {
                MainThreadScheduler.ExecuteTasks();
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
