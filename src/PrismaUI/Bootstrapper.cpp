#include "Bootstrapper.h"

#include "Cef/Browser/CefRuntime.h"
#include "Core.h"
#include "InputHandler.h"
#include "Menus/CursorMenu/CursorMenu.h"
#include "Renderer.h"

namespace PrismaUI::Bootstrapper {
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

        logger::info("PrismaUI successfully initialized");

        return true;
    }

    bool Initialize() {
        auto isInitialized = InitializeImpl();
        if (!isInitialized) {
            Shutdown();
        }

        return isInitialized;
    }

    void Shutdown() {
        logger::info("Shutdown...");
        Core::Shutdown();
        Renderer::GetSingleton().Shutdown();
        InputHandler::GetSingleton().Shutdown();
        Cef::CefRuntime::GetSingleton().Shutdown();
        logger::info("Shutdown complete");
    }
}
