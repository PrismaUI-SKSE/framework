#include "PrismaUIMenu.h"

#include "PrismaUI/Renderer.h"
#include "RE/U/UIMessage.h"

PrismaUIMenu::PrismaUIMenu() {
    menuFlags.set(RE::UI_MENU_FLAGS::kRendersUnderPauseMenu);
    menuFlags.set(RE::UI_MENU_FLAGS::kAdvancesUnderPauseMenu);
    menuFlags.set(RE::UI_MENU_FLAGS::kAlwaysOpen);
    menuFlags.set(RE::UI_MENU_FLAGS::kAllowSaving);
    menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);

    inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kNone;  // We handle our own input
    depthPriority = 11;                                      // Console - 1
}

RE::UI_MESSAGE_RESULTS PrismaUIMenu::ProcessMessage(RE::UIMessage& a_message) {
    if (a_message.menu != MENU_NAME) {
        return RE::UI_MESSAGE_RESULTS::kPassOn;
    }

    if (a_message.type == RE::UI_MESSAGE_TYPE::kHide) {
        return RE::UI_MESSAGE_RESULTS::kIgnore;
    }

    return RE::UI_MESSAGE_RESULTS::kPassOn;
}

void PrismaUIMenu::AdvanceMovie(float, std::uint32_t) {}

void PrismaUIMenu::PostDisplay() { PrismaUI::Renderer::GetSingleton().EndRender(); }

SKSE::stl::owner<RE::IMenu*> PrismaUIMenu::Creator() { return new PrismaUIMenu(); }