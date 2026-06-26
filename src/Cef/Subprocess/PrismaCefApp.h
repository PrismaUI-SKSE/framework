#pragma once

#include <string>

#include "Cef/Subprocess/PrismaCefRenderApp.h"
#include "include/cef_app.h"

namespace PrismaUI::Cef {
    class PrismaCefApp final : public CefApp {
    public:
        explicit PrismaCefApp(std::string adapterLuidValue = {});
        void OnBeforeCommandLineProcessing(const CefString& process_type,
                                           CefRefPtr<CefCommandLine> command_line) override;
        CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;

    private:
        CefRefPtr<PrismaCefRenderApp> renderHandler_;
        // "<HighPart>,<LowPart>" decimal LUID forwarded to Chromium's GPU process via
        // the use-adapter-luid switch; empty when no specific adapter must be forced.
        std::string adapterLuidValue_;

        IMPLEMENT_REFCOUNTING(PrismaCefApp);
    };

    CefRefPtr<CefApp> CreatePrismaCefApp(std::string adapterLuidValue = {});
}
