#include "Cef/PrismaCefApp.h"

namespace PrismaUI::Cef
{
    CefRefPtr<CefApp> CreatePrismaCefApp()
    {
        return new PrismaCefApp();
    }
}
