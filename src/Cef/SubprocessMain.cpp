#include <windows.h>

#include "Cef/PrismaCefApp.h"
#include "include/cef_app.h"

namespace
{
    int RunCefSubprocess(HINSTANCE instance)
    {
        CefMainArgs mainArgs(instance);
        CefRefPtr<CefApp> app = PrismaUI::Cef::CreatePrismaCefApp();

        const int exitCode = CefExecuteProcess(mainArgs, app, nullptr);
        return exitCode >= 0 ? exitCode : 0;
    }
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    return RunCefSubprocess(instance);
}
