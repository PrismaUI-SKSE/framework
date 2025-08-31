#pragma once

#include <Ultralight/Ultralight.h>

namespace WinKeyHandler {
	using namespace ultralight::KeyCodes;

	int WinKeyToUltralightKey(UINT win_key);
	std::string GetUltralightKeyIdentifier(int ul_key);
	void GetUltralightModifiers(ultralight::KeyEvent& ev);
	ultralight::KeyEvent CreateKeyEvent(ultralight::KeyEvent::Type type, WPARAM wParam, LPARAM lParam);
}