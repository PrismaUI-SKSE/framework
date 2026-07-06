#include <spdlog/sinks/basic_file_sink.h>

#include "API/API.h"
#include "Globals.h"
#include "Hooks/HookInstaller.h"
#include "Hooks/HooksLib.h"
#include "Menus/CursorMenu/CursorMenu.h"
#include "PrismaUI/Bootstrapper.h"
#include "PrismaUI/Renderer.h"
#include "PrismaUI_API.h"

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            break;
    }
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse, false);  // false = don't initialize logger by default
    logger::init();
    // pattern: [2024-01-01 12:00:00.000] [info] [1234] [sourcefile.cpp:123] Log message
    spdlog::set_pattern("[%Y-%m-%d %T.%e] [%l] [%t] [%s:%#] %v");

    logger::info("---------------- {} {} by {} ----------------", SKSE::GetPluginName(), SKSE::GetPluginVersion(),
                 SKSE::GetPluginAuthor());
    logger::info("-------------------- Docs and Guides: https://prismaui.dev -------------------");
    logger::info("------------------- built using CommonLibSSE-NG v{} -------------------", COMMONLIBSSE_VERSION);
    logger::info("------------------- Running on Skyrim v{} -------------------",
                 REL::Module::get().version().string());

    auto g_messaging =
        reinterpret_cast<SKSE::MessagingInterface*>(a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));

    if (!g_messaging) {
        logger::critical("Failed to load messaging interface! This error is fatal, plugin will not load.");
        return false;
    }

    SKSE::AllocTrampoline(1 << 10);

    g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

    MainThreadScheduler.SetThreadId(std::this_thread::get_id());

    Hooks::HookInstaller<Hooks::UpdateHook>::Install(
        [](const auto& originalFunc, RE::Main* main, float delta) {
            [[unlikely]]
            if (!PrismaUI::Bootstrapper::Initialize()) {
                logger::critical("Failed to initialize PrismaUI, exiting...");
                throw std::runtime_error("Failed to initialize PrismaUI");
            }

            MainThreadScheduler.ExecuteTasks();
            originalFunc(main, delta);
        });

    return true;
}

extern "C" DLLEXPORT void* SKSEAPI RequestPluginAPI(const PRISMA_UI_API::InterfaceVersion a_interfaceVersion) {
    auto api = PluginAPI::PrismaUIInterface::GetSingleton();

    const auto requestedVersion = static_cast<uint8_t>(a_interfaceVersion);
    switch (requestedVersion) {
        case static_cast<uint8_t>(PRISMA_UI_API::InterfaceVersion::V1):
            logger::info("RequestPluginAPI returned V1 interface for ABI epoch 4");
            return static_cast<PRISMA_UI_API::IVPrismaUI1*>(api);
        case static_cast<uint8_t>(PRISMA_UI_API::InterfaceVersion::V2):
            logger::info("RequestPluginAPI returned V2 interface for ABI epoch 5");
            return static_cast<PRISMA_UI_API::IVPrismaUI2*>(api);
        case static_cast<uint8_t>(PRISMA_UI_API::InterfaceVersion::V3):
            logger::info("RequestPluginAPI returned V3 interface for ABI epoch 6");
            return static_cast<PRISMA_UI_API::IVPrismaUI3*>(api);
        default:
            logger::info("RequestPluginAPI requested unsupported interface version {}", requestedVersion);
            return nullptr;
    }
}
