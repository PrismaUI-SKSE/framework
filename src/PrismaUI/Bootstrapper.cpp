#include "Bootstrapper.h"

#include "Cef/Browser/CefRuntime.h"
#include "Menus/CursorMenu/CursorMenu.h"

namespace PrismaUI::Bootstrapper {
    bool Initialize()
    {
        logger::info("Bootstrapper: Initializing PrismaUI...");
        auto renderManager = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderManager) {
            logger::critical("Bootstrapper: RenderManager is null!");
            return false;
        }

        auto& runtimeData = renderManager->GetRuntimeData();
        if (!runtimeData.renderWindows || !runtimeData.renderWindows->hWnd) {
            logger::critical("Bootstrapper: HWND is null!");
            return false;
        }

        auto screenSize = renderManager->GetScreenSize();

        Cef::CefRuntime::GetSingleton().Initialize(reinterpret_cast<HWND>(runtimeData.renderWindows->hWnd), screenSize.width, screenSize.height);

        CursorMenuEx::InstallHook();

        return true;
    }
}
