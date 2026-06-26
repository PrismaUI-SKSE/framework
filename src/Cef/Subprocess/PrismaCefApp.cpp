#include "Cef/Subprocess/PrismaCefApp.h"

#include <utility>

#include "Cef/Subprocess/PrismaCefRenderApp.h"
#include "include/cef_command_line.h"

namespace PrismaUI::Cef {
    PrismaCefApp::PrismaCefApp(std::string adapterLuidValue)
        : renderHandler_(new PrismaCefRenderApp()), adapterLuidValue_(std::move(adapterLuidValue)) {}

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

        // Force Chromium's GPU process onto the same D3D11 adapter Skyrim renders on. On
        // hybrid-GPU machines Chromium otherwise composites OSR output on a different
        // adapter, so OnAcceleratedPaint's shared NT texture cannot be opened on our
        // render device (OpenSharedResource1 fails with E_INVALIDARG) and nothing draws.
        // Value is "<HighPart>,<LowPart>" decimal; Chromium copies it to the GPU process.
        if (!adapterLuidValue_.empty()) {
            command_line->AppendSwitchWithValue("use-adapter-luid", adapterLuidValue_);
        }
    }

    CefRefPtr<CefRenderProcessHandler> PrismaCefApp::GetRenderProcessHandler() { return renderHandler_; }

    CefRefPtr<CefApp> CreatePrismaCefApp(std::string adapterLuidValue) {
        return new PrismaCefApp(std::move(adapterLuidValue));
    }
}
