#include <PrismaUI/PrismaUI.h>

namespace PrismaUI::WindowsKeyHandler {
	using namespace ultralight::KeyCodes;

	int WinKeyToUltralightKey(UINT win_key) {
		switch (win_key) {
		case VK_BACK: return GK_BACK;
		case VK_TAB: return GK_TAB;
		case VK_CLEAR: return GK_CLEAR;
		case VK_RETURN: return GK_RETURN;
		case VK_SHIFT: return GK_SHIFT;
		case VK_CONTROL: return GK_CONTROL;
		case VK_MENU: return GK_MENU;
		case VK_PAUSE: return GK_PAUSE;
		case VK_CAPITAL: return GK_CAPITAL;
		case VK_ESCAPE: return GK_ESCAPE;
		case VK_SPACE: return GK_SPACE;
		case VK_END: return GK_END;
		case VK_HOME: return GK_HOME;
		case VK_LEFT: return GK_LEFT;
		case VK_UP: return GK_UP;
		case VK_RIGHT: return GK_RIGHT;
		case VK_DOWN: return GK_DOWN;
		case VK_SELECT: return GK_SELECT;
		case VK_PRINT: return GK_PRINT;
		case VK_EXECUTE: return GK_EXECUTE;
		case VK_SNAPSHOT: return GK_SNAPSHOT;
		case VK_INSERT: return GK_INSERT;
		case VK_DELETE: return GK_DELETE;
		case VK_HELP: return GK_HELP;

		case '0': return GK_0;
		case '1': return GK_1;
		case '2': return GK_2;
		case '3': return GK_3;
		case '4': return GK_4;
		case '5': return GK_5;
		case '6': return GK_6;
		case '7': return GK_7;
		case '8': return GK_8;
		case '9': return GK_9;
		case 'A': return GK_A;
		case 'B': return GK_B;
		case 'C': return GK_C;
		case 'D': return GK_D;
		case 'E': return GK_E;
		case 'F': return GK_F;
		case 'G': return GK_G;
		case 'H': return GK_H;
		case 'I': return GK_I;
		case 'J': return GK_J;
		case 'K': return GK_K;
		case 'L': return GK_L;
		case 'M': return GK_M;
		case 'N': return GK_N;
		case 'O': return GK_O;
		case 'P': return GK_P;
		case 'Q': return GK_Q;
		case 'R': return GK_R;
		case 'S': return GK_S;
		case 'T': return GK_T;
		case 'U': return GK_U;
		case 'V': return GK_V;
		case 'W': return GK_W;
		case 'X': return GK_X;
		case 'Y': return GK_Y;
		case 'Z': return GK_Z;

		case VK_LWIN: return GK_LWIN;
		case VK_RWIN: return GK_RWIN;
		case VK_APPS: return GK_APPS;
		case VK_SLEEP: return GK_SLEEP;

		case VK_NUMPAD0: return GK_NUMPAD0;
		case VK_NUMPAD1: return GK_NUMPAD1;
		case VK_NUMPAD2: return GK_NUMPAD2;
		case VK_NUMPAD3: return GK_NUMPAD3;
		case VK_NUMPAD4: return GK_NUMPAD4;
		case VK_NUMPAD5: return GK_NUMPAD5;
		case VK_NUMPAD6: return GK_NUMPAD6;
		case VK_NUMPAD7: return GK_NUMPAD7;
		case VK_NUMPAD8: return GK_NUMPAD8;
		case VK_NUMPAD9: return GK_NUMPAD9;
		case VK_MULTIPLY: return GK_MULTIPLY;
		case VK_ADD: return GK_ADD;
		case VK_SEPARATOR: return GK_SEPARATOR;
		case VK_SUBTRACT: return GK_SUBTRACT;
		case VK_DECIMAL: return GK_DECIMAL;
		case VK_DIVIDE: return GK_DIVIDE;

		case VK_F1: return GK_F1;
		case VK_F2: return GK_F2;
		case VK_F3: return GK_F3;
		case VK_F4: return GK_F4;
		case VK_F5: return GK_F5;
		case VK_F6: return GK_F6;
		case VK_F7: return GK_F7;
		case VK_F8: return GK_F8;
		case VK_F9: return GK_F9;
		case VK_F10: return GK_F10;
		case VK_F11: return GK_F11;
		case VK_F12: return GK_F12;
		case VK_F13: return GK_F13;
		case VK_F14: return GK_F14;
		case VK_F15: return GK_F15;
		case VK_F16: return GK_F16;
		case VK_F17: return GK_F17;
		case VK_F18: return GK_F18;
		case VK_F19: return GK_F19;
		case VK_F20: return GK_F20;
		case VK_F21: return GK_F21;
		case VK_F22: return GK_F22;
		case VK_F23: return GK_F23;
		case VK_F24: return GK_F24;

		case VK_NUMLOCK: return GK_NUMLOCK;
		case VK_SCROLL: return GK_SCROLL;

		case VK_LSHIFT: return GK_LSHIFT;
		case VK_RSHIFT: return GK_RSHIFT;
		case VK_LCONTROL: return GK_LCONTROL;
		case VK_RCONTROL: return GK_RCONTROL;
		case VK_LMENU: return GK_LMENU;
		case VK_RMENU: return GK_RMENU;

		case VK_OEM_1:      return GK_OEM_1;
		case VK_OEM_PLUS:   return GK_OEM_PLUS;
		case VK_OEM_COMMA:  return GK_OEM_COMMA;
		case VK_OEM_MINUS:  return GK_OEM_MINUS;
		case VK_OEM_PERIOD: return GK_OEM_PERIOD;
		case VK_OEM_2:      return GK_OEM_2;
		case VK_OEM_3:      return GK_OEM_3;
		case VK_OEM_4:      return GK_OEM_4;
		case VK_OEM_5:      return GK_OEM_5;
		case VK_OEM_6:      return GK_OEM_6;
		case VK_OEM_7:      return GK_OEM_7;

		default:
			return GK_UNKNOWN;
		}
	}


	std::string GetUltralightKeyIdentifier(int ul_key) {
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "U+%04X", ul_key);
		return std::string(buffer);
	}


	void GetUltralightModifiers(ultralight::KeyEvent& ev) {
		ev.modifiers = 0;
		if (GetKeyState(VK_MENU) < 0)
			ev.modifiers |= ultralight::KeyEvent::kMod_AltKey;
		if (GetKeyState(VK_CONTROL) < 0)
			ev.modifiers |= ultralight::KeyEvent::kMod_CtrlKey;
		if (GetKeyState(VK_SHIFT) < 0)
			ev.modifiers |= ultralight::KeyEvent::kMod_ShiftKey;
		if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0)
			ev.modifiers |= ultralight::KeyEvent::kMod_MetaKey;
	}

	ultralight::KeyEvent CreateKeyEvent(ultralight::KeyEvent::Type type, WPARAM wParam, LPARAM lParam) {
		ultralight::KeyEvent ev;
		ev.type = type;
		GetUltralightModifiers(ev);

		ev.virtual_key_code = WinKeyToUltralightKey(static_cast<UINT>(wParam));

		ev.key_identifier = "";

		wchar_t name_buffer[256];
		UINT scan_code = (lParam >> 16) & 0xFF;
		if (GetKeyNameTextW(static_cast<LONG>(scan_code << 16), name_buffer, _countof(name_buffer)) > 0) {
			std::wstring ws(name_buffer);
		}

		ev.text = "";
		ev.unmodified_text = "";

		ev.is_keypad = ((HIWORD(lParam) & KF_EXTENDED) == KF_EXTENDED ||
			(wParam >= VK_NUMPAD0 && wParam <= VK_DIVIDE));
		ev.is_auto_repeat = (HIWORD(lParam) & KF_REPEAT) == KF_REPEAT;
		ev.is_system_key = (ev.modifiers & ultralight::KeyEvent::kMod_AltKey) &&
			(wParam == VK_TAB || wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_SPACE ||
				(wParam >= VK_F1 && wParam <= VK_F24));

		return ev;
	}
}
