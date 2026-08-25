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
│   ├── PrismaVR.{h,cpp}      — VR integration (OpenVR, 3D panels and lasers)
│   └── ModelPreview.{h,cpp}  — item NIF/DDS loader and offscreen 3D preview renderer
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

## 3D model previews

`MODELPREVIEW_API.md` is the view-author contract. The feature is controlled by
`PrismaUI_ModelPreview.ini`; when disabled, the JS functions are not bound.

`Communication` binds `__prismaUI_showModelPreview` and
`__prismaUI_hideModelPreview` with the calling `viewId`. A preview is keyed by
`(viewId, id)`: omitted `id` means the unnamed default slot, while stable,
distinct IDs let one view own several previews. Showing an existing key updates
that slot. Hiding with an ID removes only that slot; hiding without one and
`OnPanelDestroyed(viewId)` clear every preview owned by the view.

Threading and rendering rules:

- JS callbacks only enqueue show/hide requests under `g_reqMutex`; they must not
  mutate render state directly.
- `TickCore()` owns `g_previews` on the render thread. It drains requests,
  adopts worker results, creates per-preview color targets, and renders dirty
  previews. NIF/DDS loading runs on the single model worker.
- Every preview owns its color render target; the depth target is shared because
  previews render sequentially. Static previews render once until dirtied;
  spinning previews redraw every frame.
- Flatscreen uses `GetFlatOverlays()` to composite all ready previews into their
  owning view. VR uses one OpenVR overlay per ready preview, positioned from the
  owning panel transform, with a 32-visible-preview safety cap.
- `Shutdown()` must join the worker and release preview overlays and D3D
  resources. Keep its teardown idempotent and never submit a tearing-down view
  texture to OpenVR.

## External surface hosting (API v3)

`IVPrismaUI3` lets another renderer, such as DragonBoard, present a PrismaUI
view on its own in-world surface. `SetExternalSurfaceHost(view, true)` keeps the
Ultralight view rendering but excludes it from PrismaUI's flatscreen draw and
PrismaVR overlay/laser path. `ComposeExternalSurfaces()` copies the HTML view
and any ModelPreview sprites into the externally acquired texture.

- `AcquireSurface()` returns strong COM references plus dimensions and a
  generation counter. Consumers must call `ReleaseSurface()` and reacquire when
  the generation changes.
- Texture, SRV, dimensions, and generation are one synchronized unit. Internal
  drawing and VR submission must use `PrismaView::AcquireTextureSnapshot()`;
  never retain or combine raw texture fields outside `textureMutex`.
- External pointer APIs are valid only while external hosting is enabled. They
  may check atomic host state on the caller thread, but must touch
  `ultralightView` only inside the submitted Ultralight-thread task.
- View enumeration must copy order, ID, path, and flags while `viewsMutex` is
  held, then sort the copied values. Do not sort live `PrismaView` fields after
  releasing the lock.
- Deferred initialization is gated by DataLoaded and a shutdown generation.
  Queued SKSE tasks must not initialize the core after shutdown begins.
- SpriteBatch composition into an external render target requires a full D3D11
  pipeline-state backup and restore, including zero/multiple viewport states.

## VR laser visuals (2026-08-03)

The beam matches OCU's menu laser (`OpenOVR/Misc/Keyboard/BeamTexture.h` in the
OCU repo): tapered tip (last 30% narrows/fades to a point), parabolic soft
edges, hot core, PREMULTIPLIED alpha (OCU composites overlay quads
premultiplied — no UNPREMULTIPLIED flag on its layers). Two textures are
created up front (warm white idle `255,240,220,200`, electric blue click
`55,145,255,220`) and swapped on trigger state in `UpdateLasers()`; the CSS
cursor dot swaps to matching blue in `ProcessInput()`. Gotcha: overlay quad
height = widthMeters × (texH/texW), so `BEAM_TEX_H` is derived from
`LASER_LENGTH / LASER_WIDTH` — changing beam length/width constants without
keeping that ratio stretches the taper.

## Other gotchas

- **Pixel format:** Ultralight renders **BGRA** → D3D texture is `DXGI_FORMAT_B8G8R8A8_UNORM`. Do not swap to RGBA.
- **SEH translation** is installed on `ultralightThread`: access violations become `SEHException : std::exception`. Stack overflow is not translated — process will crash.
- **Inspector** is a second `ultralight::View` with its own texture and `inspectorBufferMutex`. Position set via `SetInspectorBounds`.
