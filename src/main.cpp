#include "API/API.h"
#include "Menus/CursorMenu/CursorMenu.h"
#include "PrismaUI_API.h"
#include "Utils/DllLoader.h"
#include <spdlog/sinks/basic_file_sink.h>

static void SKSEMessageHandler(SKSE::MessagingInterface::Message *message) {
  switch (message->type) {
  case SKSE::MessagingInterface::kDataLoaded:
    CursorMenuEx::InstallHook();
    break;
  }
}

extern "C" DLLEXPORT bool SKSEAPI
SKSEPlugin_Load(const SKSE::LoadInterface *a_skse) {

  SKSE::Init(a_skse, false); // false = don't initialize logger by default
  logger::init();
  // pattern: [2024-01-01 12:00:00.000] [info] [1234] [sourcefile.cpp:123] Log message
  spdlog::set_pattern("[%Y-%m-%d %T.%e] [%l] [%t] [%s:%#] %v");

  logger::info("---------------- {} {} by {} ----------------", SKSE::GetPluginName(), SKSE::GetPluginVersion(), SKSE::GetPluginAuthor());
  logger::info("-------------------- Docs and Guides: https://prismaui.dev -------------------");
  logger::info("------------------- built using CommonLibSSE-NG v{} -------------------", COMMONLIBSSE_VERSION);
  logger::info("------------------- Running on Skyrim v{} -------------------", REL::Module::get().version().string());

  // Load Ultralight DLLs from Data/PrismaUI/libs before any Ultralight API usage
  if (!PrismaUI::Utils::DllLoader::GetSingleton().LoadUltralightLibraries()) {
    logger::critical("Failed to load Ultralight libraries! Plugin will not load.");
    return false;
  }

  auto g_messaging = reinterpret_cast<SKSE::MessagingInterface *>(
      a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));

    if (!g_messaging) {
        logger::critical("Failed to load messaging interface! This error is fatal, plugin will not load.");
        return false;
    }

    SKSE::AllocTrampoline(1 << 10);

    g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

  return true;
}

extern "C" DLLEXPORT void *SKSEAPI
RequestPluginAPI(const PRISMA_UI_API::InterfaceVersion a_interfaceVersion) {
  auto api = PluginAPI::PrismaUIInterface::GetSingleton();

  switch (a_interfaceVersion) {
  case PRISMA_UI_API::InterfaceVersion::V1:
    logger::info("RequestPluginAPI returned V1 interface");
    return static_cast<PRISMA_UI_API::IVPrismaUI1*>(api);
  case PRISMA_UI_API::InterfaceVersion::V2:
    logger::info("RequestPluginAPI returned V2 interface");
    return static_cast<PRISMA_UI_API::IVPrismaUI2*>(api);
  default:
    logger::info("RequestPluginAPI requested unsupported interface version");
    return nullptr;
  }
}
