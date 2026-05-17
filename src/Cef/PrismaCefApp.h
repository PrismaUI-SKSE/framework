#pragma once

#include "include/cef_app.h"

namespace PrismaUI::Cef
{
    class PrismaCefApp final : public CefApp
    {
    public:
        PrismaCefApp() = default;
        void OnBeforeCommandLineProcessing(const CefString& process_type,
                                           CefRefPtr<CefCommandLine> command_line) override;

    private:
        IMPLEMENT_REFCOUNTING(PrismaCefApp);
    };

    CefRefPtr<CefApp> CreatePrismaCefApp();
}
