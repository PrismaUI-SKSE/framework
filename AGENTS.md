# PrismaUI Framework

SKSE plugin (DLL) that renders HTML/CSS/JS UI overlays in Skyrim using **Ultralight** (headless browser) + **DirectX 11** SpriteBatch.

> **For the AI agent:** After implementing any non-trivial feature or pattern, update this file — add or extend sections, note gotchas found during the work. Keep it concise and factual. Do not document what the code already says; only record things that would surprise a reader or aren't obvious from the source.

## Source layout

```text
src/
├── PrismaUI_API.h            — public modder header (copy into mod project)
├── PrismaUI/
│   ├── Core.{h,cpp}          — global state, renderer init, D3DPresent hook entry
│   ├── ViewManager.{h,cpp}   — Create/Show/Hide/Focus/Destroy views
│   ├── ViewRenderer.{h,cpp}  — Ultralight bitmap → pixel buffer → D3D11 texture → SpriteBatch
│   ├── ViewOperationQueue.{h,cpp} — per-frame operation queue (max 100 ops/view)
│   ├── Communication.{h,cpp} — JS↔C++ bridge: Invoke, RegisterJSListener, InteropCall
│   ├── InputHandler.{h,cpp}  — WndProc hook, mouse/keyboard → Ultralight events
│   ├── GamepadInputHandler.{h,cpp} — BSInputDeviceManager → Ultralight Gamepad API
│   ├── Listeners.{h,cpp}     — LoadListener / ViewListener (OnDOMReady, OnFinishLoading, console)
│   ├── Inspector.{h,cpp}     — DevTools as a separate Ultralight view rendered as overlay
│   ├── ImeHelper.{h,cpp}     — IME (CJK input) support
│   └── PrismaVR.{h,cpp}      — VR integration (OpenVR, 3D panels)
├── API/API.{h,cpp}           — IVPrismaUI1/IVPrismaUI2 implementation, exported via RequestPluginAPI
├── Hooks/Hooks.{h,cpp}       — D3DPresentHook
├── Menus/
│   ├── CursorMenu/           — Scaleform cursor menu (flatscreen)
│   └── FocusMenu/            — Scaleform blocker menu, opened on Focus() in flatscreen
└── Utils/
    ├── SingleThreadExecutor.h — priority task queue on a single thread (the Ultralight thread)
    ├── NanoID.h               — PrismaViewId (uint64_t) generator
    └── WinKeyHandler/         — WinAPI VK codes → Ultralight KeyEvent
```

## Critical threading rules

`ultralightThread` (`SingleThreadExecutor`) is the **only** thread allowed to touch any `ultralight::View`, `Renderer`, or `JSContext`. **All** Ultralight calls must go through `ultralightThread.submit()`.

Per-frame flow in `D3DPresent`:

1. `ViewOperationQueue::ProcessAllViewOperations()` — drains queued Show/Hide/Focus ops
2. `ultralightThread.submit(Renderer::Update)` — JS ticks, layout, paint
3. `ultralightThread.submit(ViewRenderer::RenderViews)` — BitmapSurface → `pixelBuffer`
4. Game thread: `pixelBuffer` → D3D11 texture (Map/Unmap), SpriteBatch draw sorted by `view->order`

## View lifecycle

```text
ViewManager::Create()   → PrismaView added to map; ultralightView = nullptr
  ↓  (per-frame on ultralightThread)
Renderer creates View   → ultralightView assigned
  ↓
OnDOMReady()            → domReadyCallback fires  ← safe to Invoke JS here
  ↓
OnFinishLoading()       → isLoadingFinished = true; BindJSCallbacks() called
```

`domReadyCallback` fires **before** `OnFinishLoading`. JS listeners bound by `RegisterJSListener` are only active after `OnFinishLoading`. They rebind automatically on every reload — no need to call `RegisterJSListener` again.

`IsValid()` only checks map membership; `ultralightView` may still be null. Always check `viewData->ultralightView` before use.

## JS ↔ C++ communication

```cpp
// C++ → JS (async, goes through ultralightThread queue)
Communication::Invoke(viewId, "fn('data')", [](std::string r) { /* result */ });

// C++ → JS (sync, call ONLY when already on ultralightThread)
Communication::InvokeFromUltralightThread(viewId, "fn()");

// JS → C++ (JS calls window.myCallback("data"))
Communication::RegisterJSListener(viewId, "myCallback", [](std::string arg) { });

// C++ → JS function call by name, single string arg — fastest path
Communication::InteropCall(viewId, "onData", jsonString);
```

## VR vs flatscreen differences

`Focus()` in flatscreen: opens `FocusMenu`, disables game controls, optionally pauses game.  
In VR (`PrismaVR::IsVRActive()`): none of that — player keeps full control. Always branch on this flag when touching focus-related logic.

## HTML file resolution

```cpp
ViewManager::Create("ui/index.html");  // → file:///views/ui/index.html
ViewManager::Create("https://...");    // passed through as-is
```

## Engine state must be touched on the main thread

`ViewOperationQueue` runs Show/Hide/Focus/Unfocus bodies on the **Ultralight thread**, not the main thread. Anything in them that touches `RE::` singletons has to be marshalled.

`SKSE::GetTaskInterface()->AddTask` is **not** reliable here — in some load orders it does not end up running on the main thread. Two mechanisms are used instead:

- `ControlMap::ToggleControls` and `ui->numPausesGame` go through `AddUITask` (`RunOnUIThread` in `ViewManager.cpp`), same as `FocusMenu::Open`/`Close`. Doing these from the Ultralight thread is what made rapid open/close of a view leave the player unable to move.
- Everything else uses `Utils/MainThreadQueue` — `Post` from any thread, drained every frame by a `BSTEventSink<RE::InputEvent*>` on `BSInputDeviceManager`, which the engine polls on the main thread. No hardcoded addresses, so it works across SE/AE/VR.

`ControlMap::ToggleControls` is the sharp edge: it synchronously dispatches `UserEventEnabled` to `PlayerControls`, so calling it off-thread drives engine state machines from the wrong thread. That produced intermittent access violations (translated to `SEHException`) that aborted the rest of the focus operation, leaving the player with controls permanently disabled — `enabledControls` is saved in the savegame, so a reload was the only recovery.

**Never disable `USER_EVENT_FLAG::kFighting`.** The engine reacts by forcing the player to sheathe a drawn weapon (same side effect as Papyrus `DisablePlayerControls(abFighting=true)`). Attack input is already suppressed while focused, because `FocusMenu` is modal and sets `inputContext = kMenuMode`.

The gameplay-control block is tracked by *owner view* (`gameplayControlsOwner`), not per view. Focus switching unfocuses the outgoing view on a later frame than it focuses the incoming one, so a per-view flag would let the outgoing view's restore land after the incoming view's block.

`ControlMapBridge::BuildJson` walks ControlMap's per-context `BSTArray`s, which the game reallocates on remap — it runs on the main thread for the same reason.

## Other gotchas

- **Pixel format:** Ultralight renders **BGRA** → D3D texture is `DXGI_FORMAT_B8G8R8A8_UNORM`. Do not swap to RGBA.
- **SEH translation** is installed on `ultralightThread`: access violations become `SEHException : std::exception`. Stack overflow is not translated — process will crash.
- **Inspector** is a second `ultralight::View` with its own texture and `inspectorBufferMutex`. Position set via `SetInspectorBounds`.
