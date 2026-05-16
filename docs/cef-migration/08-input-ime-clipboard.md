# Step 8 - Input, Clipboard, And IME

## Goal

Convert PrismaUI input delivery from Ultralight events to CEF browser host events while preserving game input capture behavior.

## Edit Scope

- `src/PrismaUI/InputHandler.*`
- `src/PrismaUI/ImeHelper.*`
- current `WinKeyHandler` replacement or CEF-specific key converter
- `src/PrismaUI/Communication.*` for IME focus tracking bridge

## Tasks

1. Replace Ultralight input event variants with CEF-compatible event data:
   - `CefMouseEvent`
   - `CefKeyEvent`
   - wheel delta values
2. Mouse move:
   - send `browser->GetHost()->SendMouseMoveEvent(event, false)`
3. Mouse buttons:
   - map left/right/middle to `CefBrowserHost::MouseButtonType`
   - send `SendMouseClickEvent(event, button, mouseUp, clickCount)`
4. Mouse wheel:
   - send `SendMouseWheelEvent(event, deltaX, deltaY)`
   - preserve `SetScrollingPixelSize` by scaling wheel values the same way current code scales Ultralight pixel scroll
5. Keyboard:
   - convert Win32 key messages to `CefKeyEvent`
   - send `KEYEVENT_RAWKEYDOWN`, `KEYEVENT_KEYUP`, and `KEYEVENT_CHAR`
   - keep shortcut handling for copy/cut/paste/select-all if CEF does not cover it reliably in OSR
6. Focus:
   - only send events when a Prisma view has native focus
   - focus the main CEF browser host
   - shell should focus the target iframe
7. Clipboard:
   - prefer CEF's native editing behavior first
   - keep existing Win32 clipboard helper as fallback for current shortcut semantics
8. IME:
   - keep the custom `prismaIME_state` event contract if public UI content depends on it
   - replace Ultralight script evaluation with CEF frame execution
   - update focus tracking script to run in the focused iframe
   - send committed text as CEF char events
9. DevTools routing:
   - do not preserve old inspector-specific routing by default
   - DevTools opens externally or in a normal CEF window
   - remove PrismaUI's inspector texture/input path entirely
10. Log input state transitions and important errors:
   - WndProc hook install/uninstall result
   - input capture enable/disable per view
   - focused view changes
   - key conversion failures
   - clipboard open/read/write failures
   - IME association changes
   - IME focus tracking install failures

## Acceptance Criteria

- Focused iframe receives mouse, wheel, keyboard, and text input.
- Unfocused or hidden views do not receive input.
- FocusMenu, cursor hiding, game pause, and control toggles behave as before.
- Clipboard text limits remain in place if the Win32 fallback is retained.
- IME state events still reach JS with the existing event name and payload structure.
- Logs can explain why an input event was dropped or which view received it when debug logging is enabled.

## Risks

- CEF key event fields are more detailed than Ultralight's wrapper. Incorrect `windows_key_code`, `native_key_code`, or modifier flags will break text input and shortcuts.
- Iframe focus can be lost after DOM changes. The shell should re-focus the target iframe after show/order changes when that view is the native focused view.
