#include "Cef/Shared/PrismaCefApp.h"

#include "Cef/Shared/PrismaCefRenderApp.h"
#include "include/cef_command_line.h"

namespace PrismaUI::Cef {
    PrismaCefApp::PrismaCefApp() : renderHandler_(new PrismaCefRenderApp()) {}

    void PrismaCefApp::OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> command_line) {
        if (!command_line) {
            return;
        }

        command_line->AppendSwitch("disable-smooth-scrolling");
        command_line->AppendSwitch("allow-file-access-from-files");
        command_line->AppendSwitch("allow-universal-access-from-files");
    }

    CefRefPtr<CefRenderProcessHandler> PrismaCefApp::GetRenderProcessHandler() { return renderHandler_; }

    CefRefPtr<CefApp> CreatePrismaCefApp() { return new PrismaCefApp(); }
}
