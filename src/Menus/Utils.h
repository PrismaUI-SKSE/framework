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

    inline bool IsMenuOpen(std::string_view menuName) {
        auto ui = RE::UI::GetSingleton();
        return ui && ui->IsMenuOpen(menuName);
    }

    // Must be called on the main UI thread (UI task, AdvanceMovie, PostDisplay, ...).
    inline void SendMenuMessage(const RE::BSFixedString& menuName, RE::UI_MESSAGE_TYPE messageType) {
        if (auto msgQ = RE::UIMessageQueue::GetSingleton()) {
            msgQ->AddMessage(menuName, messageType, nullptr);
        }
    }

    // Callable from any thread; the message is enqueued from the main UI thread.
    inline void PostMenuMessage(std::string_view menuName, RE::UI_MESSAGE_TYPE messageType) {
        SKSE::GetTaskInterface()->AddUITask(
            [name = RE::BSFixedString(menuName), messageType] { SendMenuMessage(name, messageType); });
    }

    inline void ShowMenu(std::string_view menuName) { PostMenuMessage(menuName, RE::UI_MESSAGE_TYPE::kShow); }
}
