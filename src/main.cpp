#include <Menus/CursorMenu/CursorMenu.h>
#include <PrismaUI_API.h>
#include <API/API.h>

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
    switch (message->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        CursorMenuEx::InstallHook();
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
    logger::info("---------------- PrismaUI 1.1.0 by StarkMP <discord: starkmp> ----------------");
    logger::info("-------------------- Docs and Guides: https://prismaui.dev -------------------");
    SKSE::AllocTrampoline(1 << 10);

    g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

    return true;
}

extern "C" DLLEXPORT void* SKSEAPI RequestPluginAPI(const PRISMA_UI_API::InterfaceVersion a_interfaceVersion)
{
    auto api = PluginAPI::PrismaUIInterface::GetSingleton();

    switch (a_interfaceVersion) {
    case PRISMA_UI_API::InterfaceVersion::V1:
        logger::info("RequestPluginAPI returned the API singleton");
        return static_cast<void*>(api);
    }

    logger::info("RequestPluginAPI requested the wrong interface version");

    return nullptr;
}
