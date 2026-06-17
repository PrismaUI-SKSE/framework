#include "Cef/Subprocess/PrismaCefApp.h"

#include "Cef/Subprocess/PrismaCefRenderApp.h"
#include "include/cef_command_line.h"

namespace PrismaUI::Cef {
    PrismaCefApp::PrismaCefApp() : renderHandler_(new PrismaCefRenderApp()) {}

    void PrismaCefApp::OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> command_line) {
        if (!command_line) {
            return;
        }

        // CEF/Chromium M138+ auto de-elevates when launched as admin: it relaunches the main
        // executable de-elevated and CefInitialize returns false (CefGetExitCode == 38,
        // CEF_RESULT_CODE_NORMAL_EXIT_AUTO_DE_ELEVATED). Inside SkyrimSE.exe that would try to
        // relaunch the game itself, ignoring browser_subprocess_path. Disable it so PrismaUI
        // initializes normally when the game/mod manager runs elevated.
        command_line->AppendSwitch("do-not-de-elevate");

        command_line->AppendSwitch("disable-smooth-scrolling");
        command_line->AppendSwitch("allow-file-access-from-files");
        command_line->AppendSwitch("allow-universal-access-from-files");
        command_line->AppendSwitchWithValue("use-gl", "angle");
        command_line->AppendSwitchWithValue("use-angle", "d3d11");
    }

    CefRefPtr<CefRenderProcessHandler> PrismaCefApp::GetRenderProcessHandler() { return renderHandler_; }

    CefRefPtr<CefApp> CreatePrismaCefApp() { return new PrismaCefApp(); }
}
