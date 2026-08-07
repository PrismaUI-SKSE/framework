#pragma once

#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"
#include "SKSE/API.h"

namespace PrismaUI::Menus {
    inline RE::GPtr<RE::IMenu> GetMenu(std::string_view menuName) {
        if (auto ui = RE::UI::GetSingleton()) {
            return ui->GetMenu(menuName);
        }

        return nullptr;
    }

    inline void ShowMenu(std::string_view menuName) {
        SKSE::GetTaskInterface()->AddUITask([menuName] {
            auto ui = RE::UI::GetSingleton();
            auto msgQ = RE::UIMessageQueue::GetSingleton();

            if (ui && msgQ) {
                msgQ->AddMessage(menuName, RE::UI_MESSAGE_TYPE::kShow, nullptr);
            }
        });
    }
}
