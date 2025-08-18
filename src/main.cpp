#include "PrismaUI_API.h"
#include <keyhandler/keyhandler.h>

PRISMA_UI_API::IVPrismaUI1* PrismaUI;

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
    switch (message->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        // 1. Initialize PrismaUI API
        PrismaUI = static_cast<PRISMA_UI_API::IVPrismaUI1*>(PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));

        // 2. Create view and call "Invoke" method to send JavaScript code to view when DOM is ready.
        PrismaView view = PrismaUI->CreateView("PrismaUI-Example-UI/index.html", [](PrismaView view) -> void {
            // View DOM is ready then you can use Invoke here (make sure that your JS methods are available after DOM is ready).
            logger::info("View DOM is ready {}", view);

            PrismaUI->Invoke(view, "updateFocusLabel('No. But press F3 to focus!')");
            });

        // 3. Also you could to register JS listener to handling JS methods calls.
        PrismaUI->RegisterJSListener(view, "sendDataToSKSE", [](const char* data) -> void {
            logger::info("Received data from JS: {}", data);
            });

        // Next lines is custom KEY DOWN / KEY UP realisation which bases at "src/keyhandler".
        KeyHandler::RegisterSink();
        KeyHandler* keyHandler = KeyHandler::GetSingleton();
        const uint32_t TOGGLE_FOCUS_KEY = 0x3D; // F3 key

        // Press F3 to focus/unfocus view in-game.
        KeyHandlerEvent toggleEventHandler = keyHandler->Register(TOGGLE_FOCUS_KEY, KeyEventType::KEY_DOWN, [view]() {
            auto hasFocus = PrismaUI->HasFocus(view);

            if (!hasFocus) {
                // Focus
                if (PrismaUI->Focus(view)) {
                    PrismaUI->Invoke(view, "updateFocusLabel('Yeah, it is focused! Press F3 again to unfocus.')");
                }
            }
            else {
                // Unfocus
                PrismaUI->Unfocus(view);
                PrismaUI->Invoke(view, "updateFocusLabel('Nah, it is not focused.')");
            }
            });

        // If you want to unregister the key event handlers:
        // keyHandler->Unregister(toggleEventHandler);
        break;
    }
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    REL::Module::reset();

    auto g_messaging = reinterpret_cast<SKSE::MessagingInterface*>(a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));

    if (!g_messaging) {
        logger::critical("Failed to load messaging interface! This error is fatal, plugin will not load.");
        return false;
    }

    SKSE::Init(a_skse);
    SKSE::AllocTrampoline(1 << 10);

    g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

    return true;
}
