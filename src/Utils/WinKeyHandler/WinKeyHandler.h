#pragma once

#include <Windows.h>

#include <cstdint>

#include "Cef/Browser/CefRuntime.h"

namespace WinKeyHandler {
    uint32_t GetCefModifiers();
    PrismaUI::Cef::CefInputKey CreateKeyEvent(PrismaUI::Cef::CefInputKeyType type, WPARAM wParam, LPARAM lParam,
                                              bool isSystemKey, bool focusOnEditableField);
    PrismaUI::Cef::CefInputKey CreateCharEvent(wchar_t ch, LPARAM lParam, bool isSystemKey, bool focusOnEditableField);
}
