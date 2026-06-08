#pragma once

#include "Cef/Shared/PrismaCefRenderApp.h"
#include "include/cef_app.h"

namespace PrismaUI::Cef {
    class PrismaCefApp final : public CefApp {
    public:
        PrismaCefApp();
        void OnBeforeCommandLineProcessing(const CefString& process_type,
                                           CefRefPtr<CefCommandLine> command_line) override;
        CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;

    private:
        CefRefPtr<PrismaCefRenderApp> renderHandler_;

        IMPLEMENT_REFCOUNTING(PrismaCefApp);
    };

    CefRefPtr<CefApp> CreatePrismaCefApp();
}
