#include "WinKeyHandler.h"

#include "include/internal/cef_types.h"

namespace WinKeyHandler {
    namespace {
        bool IsKeyDown(int virtualKey) { return (GetKeyState(virtualKey) & 0x8000) != 0; }

        bool IsToggleOn(int virtualKey) { return (GetKeyState(virtualKey) & 0x0001) != 0; }

        bool IsKeypadKey(WPARAM wParam, LPARAM lParam) {
            switch (wParam) {
                case VK_NUMPAD0:
                case VK_NUMPAD1:
                case VK_NUMPAD2:
                case VK_NUMPAD3:
                case VK_NUMPAD4:
                case VK_NUMPAD5:
                case VK_NUMPAD6:
                case VK_NUMPAD7:
                case VK_NUMPAD8:
                case VK_NUMPAD9:
                case VK_MULTIPLY:
                case VK_ADD:
                case VK_SEPARATOR:
                case VK_SUBTRACT:
                case VK_DECIMAL:
                case VK_DIVIDE:
                    return true;
                case VK_INSERT:
                case VK_DELETE:
                case VK_HOME:
                case VK_END:
                case VK_PRIOR:
                case VK_NEXT:
                case VK_LEFT:
                case VK_RIGHT:
                case VK_UP:
                case VK_DOWN:
                    return (lParam & (1LL << 24)) == 0;
                default:
                    return false;
            }
        }
    }

    uint32_t GetCefModifiers() {
        uint32_t modifiers = EVENTFLAG_NONE;
        if (IsKeyDown(VK_SHIFT)) modifiers |= EVENTFLAG_SHIFT_DOWN;
        if (IsKeyDown(VK_CONTROL)) modifiers |= EVENTFLAG_CONTROL_DOWN;
        if (IsKeyDown(VK_MENU)) modifiers |= EVENTFLAG_ALT_DOWN;
        if (IsKeyDown(VK_LWIN) || IsKeyDown(VK_RWIN)) modifiers |= EVENTFLAG_COMMAND_DOWN;
        if (IsToggleOn(VK_CAPITAL)) modifiers |= EVENTFLAG_CAPS_LOCK_ON;
        if (IsToggleOn(VK_NUMLOCK)) modifiers |= EVENTFLAG_NUM_LOCK_ON;
        if (IsKeyDown(VK_RMENU) && IsKeyDown(VK_CONTROL)) modifiers |= EVENTFLAG_ALTGR_DOWN;
        return modifiers;
    }

    PrismaUI::Cef::CefInputKey CreateKeyEvent(PrismaUI::Cef::CefInputKeyType type, WPARAM wParam, LPARAM lParam,
                                              bool isSystemKey, bool focusOnEditableField) {
        PrismaUI::Cef::CefInputKey event;
        event.type = type;
        event.modifiers = GetCefModifiers();
        if (IsKeypadKey(wParam, lParam)) event.modifiers |= EVENTFLAG_IS_KEY_PAD;
        if ((lParam & (1LL << 30)) != 0) event.modifiers |= EVENTFLAG_IS_REPEAT;
        event.windowsKeyCode = static_cast<int>(wParam);
        event.nativeKeyCode = static_cast<int>(lParam);
        event.character = 0;
        event.unmodifiedCharacter = 0;
        event.isSystemKey = isSystemKey;
        event.focusOnEditableField = focusOnEditableField;
        return event;
    }

    PrismaUI::Cef::CefInputKey CreateCharEvent(wchar_t ch, LPARAM lParam, bool isSystemKey, bool focusOnEditableField) {
        PrismaUI::Cef::CefInputKey event;
        event.type = PrismaUI::Cef::CefInputKeyType::Char;
        event.modifiers = GetCefModifiers();
        if ((lParam & (1LL << 30)) != 0) event.modifiers |= EVENTFLAG_IS_REPEAT;
        event.windowsKeyCode = static_cast<int>(ch);
        event.nativeKeyCode = static_cast<int>(lParam);
        event.character = static_cast<char16_t>(ch);
        event.unmodifiedCharacter = static_cast<char16_t>(ch);
        event.isSystemKey = isSystemKey;
        event.focusOnEditableField = focusOnEditableField;
        return event;
    }
}
