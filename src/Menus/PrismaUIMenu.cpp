#include "PrismaUIMenu.h"

#include "PrismaUI/Renderer.h"
#include "RE/C/ControlMap.h"
#include "RE/C/CursorMenu.h"
#include "RE/U/UIMessage.h"
#include "Utils.h"

namespace PrismaUI::Menus {
    namespace {
        // The visible arrow is drawn by the vanilla "Cursor Menu"; kUsesCursor on its own draws nothing.
        // Vanilla menus request/release that menu from IMenu::RefreshPlatform when their cursor usage
        // changes (see external/commonlibsse-ng/src/RE/I/IMenu.cpp:75-93). PrismaUIMenu is always open, so
        // its cursor usage changes on focus instead of on open/close and it has to post those messages
        // itself.
        constexpr std::string_view CURSOR_MENU_NAME = RE::CursorMenu::MENU_NAME;
        constexpr RE::UI_MESSAGE_TYPE MENU_FOCUS_EVENT = static_cast<RE::UI_MESSAGE_TYPE>(14);
        constexpr RE::UI_MESSAGE_TYPE MENU_UNFOCUS_EVENT = static_cast<RE::UI_MESSAGE_TYPE>(15);

        bool AnyMenuUsesCursor() {
            auto ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }

            return std::ranges::any_of(ui->menuStack, [](const auto& menu) { return menu && menu->UsesCursor(); });
        }

        void RequestVanillaCursor() {
            SendMenuMessage(CURSOR_MENU_NAME,
                            IsMenuOpen(CURSOR_MENU_NAME) ? RE::UI_MESSAGE_TYPE::kUpdate : RE::UI_MESSAGE_TYPE::kShow);
        }

        void ReleaseVanillaCursor() {
            // Another open menu (console, inventory, ...) may still need the cursor; leave it to vanilla then.
            if (!AnyMenuUsesCursor()) {
                SendMenuMessage(CURSOR_MENU_NAME, RE::UI_MESSAGE_TYPE::kHide);
            }
        }

        // The engine pushes a menu's inputContext onto ControlMap when the menu opens and pops it when it
        // closes. PrismaUIMenu is always open and only becomes input-hungry on focus, so it has to move the
        // context itself. Without this the gameplay context stays on top, ESC keeps mapping to the "Pause"
        // user event and MenuControls' MenuOpenHandler turns it into the vanilla pause menu on top of the
        // focused view; in menu mode the same key maps to "Cancel", which opens nothing.
        void SetMenuModeContext(bool active) {
            auto controlMap = RE::ControlMap::GetSingleton();
            if (!controlMap) {
                return;
            }

            if (active) {
                controlMap->PushInputContext(RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode);
            } else {
                controlMap->PopInputContext(RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode);
            }
        }
    }

    PrismaUIMenu::PrismaUIMenu() {
        menuFlags.set(RE::UI_MENU_FLAGS::kRendersUnderPauseMenu);
        menuFlags.set(RE::UI_MENU_FLAGS::kAdvancesUnderPauseMenu);
        menuFlags.set(RE::UI_MENU_FLAGS::kAlwaysOpen);
        menuFlags.set(RE::UI_MENU_FLAGS::kAllowSaving);

        inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kNone;
        depthPriority = 11;  // Console - 1
    }

    RE::UI_MESSAGE_RESULTS PrismaUIMenu::ProcessMessage(RE::UIMessage& a_message) {
        if (a_message.type == RE::UI_MESSAGE_TYPE::kScaleformEvent && _isFocused) {
            return RE::UI_MESSAGE_RESULTS::kHandled;
        }

        if (a_message.menu != MENU_NAME) {
            return RE::UI_MESSAGE_RESULTS::kPassOn;
        }

        if (a_message.type == RE::UI_MESSAGE_TYPE::kHide) {
            return RE::UI_MESSAGE_RESULTS::kIgnore;
        }

        // Both transitions are edge-triggered: focus is handed between Prisma views without an unfocus in
        // between, and the pushed input context must stay balanced against the pop.
        if (a_message.type == MENU_FOCUS_EVENT && !_isFocused) {
            menuFlags.set(RE::UI_MENU_FLAGS::kModal, RE::UI_MENU_FLAGS::kUsesCursor);
            inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kMenuMode;
            _isFocused = true;
            SetMenuModeContext(true);
            RequestVanillaCursor();
        } else if (a_message.type == MENU_UNFOCUS_EVENT && _isFocused) {
            // Clear kUsesCursor first so this menu is not counted as a cursor user by the release check.
            menuFlags.reset(RE::UI_MENU_FLAGS::kModal, RE::UI_MENU_FLAGS::kUsesCursor);
            inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kNone;
            _isFocused = false;
            SetMenuModeContext(false);
            ReleaseVanillaCursor();
        }

        return RE::UI_MESSAGE_RESULTS::kHandled;
    }

    void PrismaUIMenu::AdvanceMovie(float, std::uint32_t) {
        // Vanilla hides "Cursor Menu" when a cursor-using menu closes (the console, for example), which
        // would drop the cursor while a Prisma view still holds focus. Re-request it; never force it
        // hidden here - releasing is done once, on unfocus.
        if (_isFocused && !IsMenuOpen(CURSOR_MENU_NAME)) {
            SendMenuMessage(CURSOR_MENU_NAME, RE::UI_MESSAGE_TYPE::kShow);
        }
    }

    void PrismaUIMenu::PostDisplay() { Renderer::GetSingleton().EndRender(); }

    void PrismaUIMenu::Focus() { PostMenuMessage(MENU_NAME, MENU_FOCUS_EVENT); }

    void PrismaUIMenu::Unfocus() { PostMenuMessage(MENU_NAME, MENU_UNFOCUS_EVENT); }

    SKSE::stl::owner<RE::IMenu*> PrismaUIMenu::Creator() { return new PrismaUIMenu(); }
}
