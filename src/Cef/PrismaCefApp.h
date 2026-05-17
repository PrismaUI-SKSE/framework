#pragma once

#include "include/cef_app.h"

namespace PrismaUI::Cef
{
    class PrismaCefApp final : public CefApp
    {
    public:
        PrismaCefApp() = default;

    private:
        IMPLEMENT_REFCOUNTING(PrismaCefApp);
    };

    CefRefPtr<CefApp> CreatePrismaCefApp();
}
