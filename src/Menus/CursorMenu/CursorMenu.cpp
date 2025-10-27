#include "CursorMenu.h"
#include <PrismaUI/ViewManager.h>

void CursorMenuEx::AdvanceMovie_Hook(float a_interval, std::uint32_t a_currentTime)
{
	auto ui = RE::UI::GetSingleton();

	if (ui) {
		auto cursorMenu = ui->GetMenu(RE::CursorMenu::MENU_NAME);

		CursorMenuEx* menu = static_cast<CursorMenuEx*>(cursorMenu.get());

		auto hasAnyFocus = PrismaUI::ViewManager::HasAnyActiveFocus();

		// forced setting vanilla cursor state
		if (hasAnyFocus && cursorMenu && cursorMenu->uiMovie && cursorMenu->uiMovie->GetVisible() == true) {
			cursorMenu->uiMovie->SetVisible(false);
		} else if (!hasAnyFocus && cursorMenu && cursorMenu->uiMovie && cursorMenu->uiMovie->GetVisible() == false) {
			cursorMenu->uiMovie->SetVisible(true);
		}

		return menu->_AdvanceMovie(menu, a_interval, a_currentTime);
	}
}

RE::UI_MESSAGE_RESULTS CursorMenuEx::ProcessMessage_Hook(RE::UIMessage& a_message)
{
	if (a_message.type == RE::UI_MESSAGE_TYPE::kHide) {
		if (PrismaUI::ViewManager::HasAnyActiveFocus()) {
			return RE::UI_MESSAGE_RESULTS::kIgnore;
		}

		auto ui = RE::UI::GetSingleton();

		if (ui && ui->IsMenuOpen(RE::Console::MENU_NAME)) {
			return RE::UI_MESSAGE_RESULTS::kIgnore;
		}
	}

	return _ProcessMessage(this, a_message);
}

void CursorMenuEx::InstallHook()
{
	REL::Relocation<std::uintptr_t> vTable(RE::VTABLE_CursorMenu[0]);
	_ProcessMessage = vTable.write_vfunc(0x4, &CursorMenuEx::ProcessMessage_Hook);
	_AdvanceMovie = vTable.write_vfunc(0x5, &CursorMenuEx::AdvanceMovie_Hook);
}
