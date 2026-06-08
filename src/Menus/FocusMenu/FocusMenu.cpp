#include "FocusMenu.h"

RE::stl::owner<RE::IMenu*> FocusMenu::Creator() {
    auto menu = new FocusMenu();
    if (!menu->IsValid()) {
        delete menu;
        return nullptr;
    }
    return menu;
}

FocusMenu::FocusMenu() {
    using Context = RE::UserEvents::INPUT_CONTEXT_ID;
    using MenuFlag = RE::UI_MENU_FLAGS;

    auto scaleformManager = RE::BSScaleformManager::GetSingleton();
    if (!scaleformManager) {
        logger::error("FocusMenu: BSScaleformManager singleton is null");
        return;
    }

    const bool success =
        scaleformManager->LoadMovieEx(this, "cursormenu", []([[maybe_unused]] RE::GFxMovieDef* a_def) -> void {});

    if (!success || !this->uiMovie) {
        logger::error("FocusMenu: Failed to load cursormenu movie");
        return;
    }

    _view = this->uiMovie;
    _view->SetMouseCursorCount(1);
    _view->SetVisible(false);

    this->menuFlags.set(MenuFlag::kUsesCursor, MenuFlag::kModal, MenuFlag::kAllowSaving,
                        MenuFlag::kAdvancesUnderPauseMenu, MenuFlag::kRendersUnderPauseMenu);
    this->depthPriority = 13;
    this->inputContext = Context::kMenuMode;
}

void FocusMenu::AdvanceMovie([[maybe_unused]] float a_interval, [[maybe_unused]] std::uint32_t a_currentTime) {}

RE::UI_MESSAGE_RESULTS FocusMenu::ProcessMessage(RE::UIMessage& a_message) {
    if (a_message.menu == FocusMenu::MENU_NAME) {
        return RE::UI_MESSAGE_RESULTS::kHandled;
    }
    return RE::UI_MESSAGE_RESULTS::kPassOn;
}

bool FocusMenu::IsOpen() {
    auto ui = RE::UI::GetSingleton();
    return ui && ui->IsMenuOpen(MENU_NAME);
}

void FocusMenu::Open() {
    SKSE::GetTaskInterface()->AddUITask([=] {
        auto ui = RE::UI::GetSingleton();
        auto msgQ = RE::UIMessageQueue::GetSingleton();

        if (ui && msgQ && !IsOpen()) {
            msgQ->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
    });
}

void FocusMenu::Close() {
    SKSE::GetTaskInterface()->AddUITask([=] {
        auto ui = RE::UI::GetSingleton();
        auto msgQ = RE::UIMessageQueue::GetSingleton();

        if (ui && msgQ && IsOpen()) {
            msgQ->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    });
}
