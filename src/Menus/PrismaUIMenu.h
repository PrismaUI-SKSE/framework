#pragma once

#include <atomic>
#include <cstdint>

#include "RE/I/IMenu.h"
#include "SKSE/Impl/PCH.h"

namespace PrismaUI::Menus {
    class PrismaUIMenu : RE::IMenu {
    public:
        static constexpr const char* MENU_NAME = "PrismaUI_Menu";

        PrismaUIMenu();

        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override;
        void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override;
        void PostDisplay() override;

        static void Focus();
        static void Unfocus();

        static SKSE::stl::owner<RE::IMenu*> Creator();

    private:
        // True while a Prisma view holds focus, i.e. while PrismaUI wants the vanilla cursor on screen.
        static inline std::atomic_bool _cursorRequested{false};
    };
}
