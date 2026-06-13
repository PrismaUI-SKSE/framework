# PrismaUI Agent Notes

This repository is an SKSE plugin for Skyrim that exposes a C API for mods to render HTML/CSS/JS UI through Chromium Embedded Framework (CEF). The core implementation is C++23, CommonLibSSE-NG, D3D11, DirectXTK `SpriteBatch`, and CEF 147 (Chromium 147). Ultralight is no longer used.

## Working Rules

- Keep changes scoped. This plugin runs inside Skyrim's process, so avoid broad refactors unless the task explicitly calls for them.
- Preserve the public ABI in `src/PrismaUI_API.h` and the matching vtable method order in `src/API/API.h`. Supported `InterfaceVersion` values are `V1 = 0`, `V2 = 1`, `V3 = 2`. Any other requested version (3 and up) must keep returning `nullptr`.
- Treat CEF browser/frame/V8 objects as CEF-UI-thread-owned. Route work through `Cef::CefRuntime` and `CefPostTask(TID_UI, ...)`; CEF renderer-process V8 work belongs in `PrismaCefRenderApp` on `TID_RENDERER`.
- Treat D3D11 texture/resource work as render-thread work. Texture creation, mapping, drawing, and release happen on the present/render path; the only drawn surface is the CEF overlay texture (plus the Prisma cursor).
- Public API callbacks into mods must be scheduled with `SKSE::GetTaskInterface()->AddTask`/`AddUITask` — do not call mod code directly from CEF UI/renderer threads.
- Do not directly touch `external/commonlibsse-ng` unless the task is explicitly about vendored CommonLibSSE.
- Use the harness `search` tool for content lookup and `find` for filename lookup. The `external/` and `build/external_builds/` trees are large; scope searches to `src`, `cmake`, `assets`, `docs`, and root docs unless dependency code is relevant.
- Use existing logging style via `logger::info/warn/error/debug/critical`.
- Follow `.clang-format`: Google base style, 4-space indents, no tabs, 120-column limit.
- After C++ changes, run `.\Format-Code.ps1` when `clang-format` is installed. Use `.\Format-Code.ps1 -Check` to verify formatting without rewriting files.
- Default to ASCII for new code/docs unless updating text that already uses non-ASCII.
- Avoid code duplication. If you see a pattern or that you duplicate a code, try to refactor it and extract a code to a class/function.

## Build And Packaging

- Use direct CMake preset commands from a VS 2022 Developer PowerShell or Developer Command Prompt.
- Debug configure/build (default):
  - `cmake -S . --preset=debug`
  - `cmake --build --preset=debug --parallel 8`
- Release configure/build:
  - `cmake -S . --preset=release`
  - `cmake --build --preset=release --parallel 8`
- `BuildRelease.ps1` exists as a convenience wrapper, but do not use it as the default build instruction.
- Requires `VCPKG_ROOT`, Ninja, VS 2022 C++ tooling, Node.js with `npm` on `PATH` (CMake `find_program(... npm ... REQUIRED)` fails configure without it), and `external/cef_binary_147.0.14+g76d2442+chromium-147.0.7727.138_windows64.tar.bz2` (or `PRISMAUI_CEF_ARCHIVE` pointing at an equivalent local archive).
- CMake extracts CEF to `build/external_builds/cef/<cef_binary_root>/`.
- Two targets are built:
  - `PrismaUI` (the SKSE plugin DLL) → `build/<preset>/bin/PrismaUI.dll`.
  - `PrismaUICefSubprocess` (the CEF helper executable) → `build/<preset>/bin/PrismaUICefSubprocess.exe`.
- Distribution output: `dist/PrismaUI_<version>[_Debug]/`:
  - `PrismaUI/libs/` — `libcef.dll`, `chrome_elf.dll`, `icudtl.dat`, `v8_context_snapshot.bin`, resource `.pak`s, `locales/`, ANGLE/SwiftShader/D3D support DLLs, and `PrismaUICefSubprocess.exe`.
  - `PrismaUI/shell/` — the CEF shell page, built from the `shell/app` TypeScript/Vite project into `shell/dist` and copied from there (NOT from `assets/`).
  - `SKSE/plugins/PrismaUI.dll`.
  - `NOTICES.txt` at the package root.
- The packaging step calls `cmake -E remove_directory` on the version dir before repopulating it, so stale Ultralight/per-view inspector folders never reappear; do not reintroduce Ultralight copy rules.
- The CEF shell page is a separate frontend build: the `PrismaUIShell` CMake target runs `npm ci` + `npm run build` (`tsc --noEmit` typecheck then `vite build`) in `shell/app`, emitting to `shell/dist`. Packaging copies `assets/` and `shell/dist/` into the distribution as two distinct steps.
- `UpdateExternalDeps.ps1` prepares submodules. It can remove/reinitialize submodule folders, so do not run it casually in a dirty tree.

## Repository Map

- `src/main.cpp`: SKSE entry point, logger setup, SKSE messaging, API export. No DLL preloading — CEF is loaded lazily by `Cef::CefRuntime` through `DllLoader::LoadCefLibraries()`.
- `src/PrismaUI_API.h`: public modder-facing API. Mods may copy this header.
- `src/API/`: implementation of the exported PrismaUI API singleton; DevTools methods (`OpenDevTools`/`CloseDevTools`/`IsDevToolsOpen`) live in `IVPrismaUI1` after `GetOrder` and before `HasAnyActiveFocus`.
- `src/Cef/Browser/CefRuntime.*`: CEF process/runtime lifecycle, OSR shell browser, shell-page command bus, JS invoke result bridge, and DevTools control. Linked into `PrismaUI.dll` only.
- `src/Cef/Browser/CefOsrClient.*`: `CefClient` for the shell browser; OSR `OnPaint`/`OnAcceleratedPaint`, load/render-handler glue. Linked into `PrismaUI.dll` only.
- `src/Cef/Browser/OverlayTexture.*`: D3D11 overlay texture owner for CEF accelerated shared-texture copies and CPU `OnPaint` BGRA uploads. Linked into `PrismaUI.dll` only.
- `src/Cef/Subprocess/PrismaCefApp.*`: `CefApp` returned in both processes. Linked into `PrismaUI.dll` and `PrismaUICefSubprocess.exe`.
- `src/Cef/Subprocess/PrismaCefRenderApp.*`: renderer-process `CefRenderProcessHandler`; owns V8 bindings for `Invoke`/`InteropCall`/`RegisterJSListener`/DOM-ready/console. Linked into both processes; only executes in the renderer subprocess.
- `src/Cef/Subprocess/SubprocessMain.cpp`: entry point for `PrismaUICefSubprocess.exe`.
- `src/Cef/Shared/BrowserToRendererMessages.h` / `RendererToBrowserMessages.h`: stable CEF IPC message names plus typed factories/parsers for browser-to-renderer and renderer-to-browser traffic.
- `src/Cef/Shared/ProcessMessageNames.h`: non-process-message bridge constants shared between browser and renderer.
- `src/Cef/Shared/CefUtils.h` / `ViewUtils.h`: shared CEF/V8/process-message helpers and iframe-name view-id parsing.
- `src/PrismaUI/Core.*`: global runtime state, D3D present hook, CEF lifecycle wiring, render loop, shutdown.
- `src/PrismaUI/ViewManager.*`: view lifecycle, show/hide/focus/unfocus, order, destroy, console callbacks; routes through `CefRuntime` shell commands.
- `src/PrismaUI/ViewRenderer.*`: CEF overlay draw + cursor draw on the present path. No per-view textures.
- `src/PrismaUI/InputHandler.*`: Win32 subclass hook, Skyrim input sink, mouse/key/scroll queueing, clipboard, IME focus tracking; dispatches events as CEF input events through `CefRuntime`.
- `src/PrismaUI/ImeHelper.*`: Windows IME context management and custom JS-dispatched composition/candidate state.
- `src/PrismaUI/Communication.*`: JS eval, native-to-JS calls, JS-to-C++ callback binding; backed by CEF process messages.
- `src/PrismaUI/ViewOperationQueue.*`: per-view operation queues processed from the present loop. Each `ProcessNextOperation` runs inline on the present thread (no separate executor).
- `src/Hooks/`: trampoline install wrappers for D3D hooks.
- `src/Menus/FocusMenu/`: hidden Scaleform menu used to capture UI focus and cursor behavior.
- `src/Menus/CursorMenu/`: hook that hides vanilla cursor menu while PrismaUI has active focus.
- `src/Utils/`: `DllLoader` (CEF only), encoding helpers, NanoID, `WinKeyHandler` (Win32→`CefKeyEvent`).
- `assets/`: static files copied into the `Data/PrismaUI` distribution (cursor texture and other CEF-facing assets). The shell page is NOT here; it is built from `shell/app` (see below).
- `cmake/`: `commonlibsse.cmake`, `cef.cmake`, `ExternalDependencies.cmake`, `CompilerFlags.cmake`. There is no `ultralight.cmake`.
- `shell/app/`: the CEF shell page frontend — a standalone TypeScript + Vite project (`prismaui-cef-shell`). `src/main.ts` bootstraps the page; `src/shell.ts` builds the `PrismaShellApi` command bus (`createPrismaShell`) that `CefRuntime` drives to create/show/hide/focus/order iframes named with decimal `PrismaView` ids; `src/types.ts` holds the shared TS types; `src/style.css` and `index.html` are the page shell. Built via `npm run build` into `shell/dist` (the `PrismaUIShell` target); `shell/dist` is generated output, not source. Edit the shell command bus here, not in `assets/`.

## Runtime Architecture

### Plugin Load

`SKSEPlugin_Load` in `src/main.cpp`:

- initializes SKSE and logging
- registers `SKSEMessageHandler`
- allocates trampoline storage

No CEF DLLs are loaded here; CEF is brought up lazily on first view creation. On `kDataLoaded`, `CursorMenuEx::InstallHook()` hooks Skyrim's cursor menu.

`RequestPluginAPI` returns one of `IVPrismaUI1`, `IVPrismaUI2`, or `IVPrismaUI3` implemented by `PluginAPI::PrismaUIInterface`. `InterfaceVersion` `V1` (0), `V2` (1), and `V3` (2) return the matching interface; any other numeric value is rejected with `nullptr`.

### Core Initialization

The first `ViewManager::Create()` lazily initializes the core:

- installs the D3D present hook
- registers `FocusMenu`

Graphics state is acquired lazily in `Core::InitGraphics()` from `RE::BSGraphics::Renderer`. Once `d3dDevice`/`d3dContext`/`hWnd`/screen size are valid, `Core::InitGraphics()` calls `Cef::CefRuntime::Initialize(...)` which loads `libcef.dll` (via `DllLoader::LoadCefLibraries()`), launches `PrismaUICefSubprocess.exe`, runs `CefInitialize`, and creates the single transparent OSR shell browser.

### Frame Loop

`Core::D3DPresent()` calls the original present function first, then:

- initializes graphics if needed
- detects screen-size changes and forwards them to `CefRuntime::Resize`
- calls `CefRuntime::BeginFrame()` and `CefRuntime::UpdateOverlayTexture()`
- processes all queued per-view operations
- processes the queued input events into the focused view
- draws the CEF overlay texture
- draws the Prisma cursor last

There is no Ultralight thread, renderer, or per-frame bitmap copy step.

### View Rendering And Compositing

All Prisma views are iframes inside the single CEF shell browser. The browser renders OSR into one CEF overlay texture; PrismaUI does not own per-view textures.

Shell view rules:

- Each `PrismaView` has a stable `iframeName` equal to its decimal view id and a resolved URL.
- `ViewManager` issues shell commands (create/show/hide/focus/blur/order/destroy) through `Cef::CefRuntime` which marshals them to the shell page's JS as a command bus.
- Visibility, order, and focus are DOM/CSS state on the shell page. Native side keeps the source-of-truth flags in `Core::PrismaView`.

Ordering rule:

- lower `order` draws earlier (z-index lower) and appears underneath
- higher `order` draws later (z-index higher) and appears on top
- new views default to `max(existing order) + 1`
- mods change order through `SetOrder`, which updates native state and posts a shell `z-index` command

There is no C++ per-view rectangle, transform, or clipping system. Positioning is authored in each iframe's HTML/CSS inside a full-screen transparent shell page.

### Thread Ownership

- CEF browser/frame/V8 work must run on the CEF UI thread (browser-process) or `TID_RENDERER` (renderer-process). Reach those threads through `CefRuntime` helpers and `CefPostTask`.
- Per-view operations queued through `ViewOperationQueue` run inline on the D3D present thread; they MUST NOT call CEF browser APIs directly — go through `CefRuntime` which posts to `TID_UI`.
- Public API callbacks back into mods are scheduled with `SKSE::GetTaskInterface()->AddTask`/`AddUITask`.
- Input events are collected from Win32/Skyrim callbacks, queued under mutex, and dispatched to the focused iframe through `CefRuntime::DispatchInputEvents` on the present thread.
- D3D11 textures and DirectXTK drawing are handled from the D3D present/render path. CEF accelerated-paint shared textures are imported into the same context.
- Shared view state is guarded by `Core::viewsMutex`; per-view operation queues have their own mutexes.

## Public API Notes

- `PrismaView` is a `uint64_t` ID, not a pointer.
- V1 API (`InterfaceVersion::V1 = 0`): create/destroy, invoke JS, interop call, JS listener registration, focus, visibility, scroll size, order, DevTools control, active-focus query.
- V2 API (`InterfaceVersion::V2 = 1`) adds `RegisterConsoleCallback`.
- V3 API (`InterfaceVersion::V3 = 2`) adds state-aware callback variants. The caller owns `callbackState`; PrismaUI stores and passes it back unchanged.
- `CreateView` accepts `http://` and `https://` URLs directly. Other paths become `file:///views/<htmlPath>` and are resolved by CEF against the shell base, rooted at `Data/PrismaUI`.
- `Invoke` evaluates a JS string in the target iframe and optionally returns a string result via the CEF result bridge.
- `InteropCall` calls a named global JS function with one string argument inside the target iframe and avoids script construction overhead.
- `RegisterJSListener` exposes a global JS function with the requested name inside the target iframe; JS should call it with a string argument.
- `OpenDevTools`/`CloseDevTools`/`IsDevToolsOpen` operate on the single shell browser; individual Prisma views show up as iframes named with their decimal `PrismaView` ids in the DevTools frame tree.
- API string inputs are validated as UTF-8 and converted from the system ANSI code page as fallback.

## Input And Focus

- Only the focused Prisma view receives input events.
- `Focus(view, pauseGame, disableFocusMenu)` sets native focus and posts focus to the iframe through the shell command bus, enables input capture, opens `FocusMenu` unless disabled, disables several Skyrim controls, and optionally increments `RE::UI::numPausesGame`.
- Focusing one view queues unfocus operations for any other focused views.
- `Unfocus`, `Hide`, and `Destroy` clean up capture and pause state and blur the iframe in CEF.
- `FocusMenu` is a hidden Scaleform menu using cursor/modal flags to keep the game in menu-mode input while PrismaUI is active.
- `CursorMenuEx` hides the vanilla cursor menu while any PrismaUI view has focus.

## IME And Clipboard

- `InputHandler` subclasses the game HWND with `SetWindowSubclass`.
- Keyboard input uses `WinKeyHandler` to convert Win32 messages into `CefKeyEvent`s, which are queued and dispatched through `CefRuntime`.
- Clipboard reads/writes use Unicode clipboard text, with hard safety limits.
- IME is intentionally custom: native IME windows are suppressed and IME state is dispatched to JS as a `prismaIME_state` custom event by running script in the focused iframe.
- UI authors are expected to render their own IME overlay from that event. See `IME_SUPPORT.md`.

## DevTools

- DevTools is browser-wide: there is exactly one DevTools window for the PrismaUI shell browser, opened via `IVPrismaUI1::OpenDevTools`.
- All Prisma views are visible in the DevTools frame tree as iframes named with their decimal `PrismaView` ids.
- There is no per-view inspector, no embedded inspector surface, and no per-view inspector texture. The old `CreateInspectorView`/`SetInspectorVisibility`/`IsInspectorVisible`/`SetInspectorBounds` methods are intentionally absent from the public header.

## Common Pitfalls

- Do not call CEF browser/frame/V8 methods directly from arbitrary threads. Go through `CefRuntime` (browser-process UI thread) or `PrismaCefRenderApp` (renderer-process `TID_RENDERER`).
- Do not release or recreate D3D resources from a CEF thread; do it on the present/render path.
- Do not reintroduce per-view textures, bitmap-surface code, or an Ultralight-style render loop. The drawn surface is the single CEF overlay plus the cursor.
- Be careful when adding fields to `PrismaView`; consider shutdown, destroy, in-flight operation-queue entries, and CEF iframe readiness.
- Do not change `PrismaUI_API.h` method order or insert methods into earlier interfaces. Add new API only by extending a new interface version.
- `ViewOperationQueue` executes at most one operation per view per present-loop pass. Long synchronous operations on the present thread cause stutter; offload work into `CefPostTask` instead.
- `Utils::GetBasePath()` uses `std::filesystem::current_path() / "Data" / "PrismaUI"`, which is correct for the in-game working directory but not for arbitrary process working directories.
- CEF `RegisterJSListener` and `Invoke` result delivery cross process boundaries; assume failures (frame gone, renderer crash, race with destroy) and ensure callbacks fire exactly once with a sensible payload.

## Verification

There is no dedicated test suite in this repository. For code changes:

- At minimum, run `cmake -S . --preset=debug` and `cmake --build --preset=debug --parallel 8` when the local environment has VS, vcpkg, Ninja, and the CEF binary archive.
- For release/perf-sensitive work, also build `release`.
- For rendering/input/IME/DevTools/CEF-lifecycle changes, static build success is not enough; verify in-game when possible. Look for `CefInitialize` success, subprocess path, shell browser creation, selected GPU/CPU paint path, and clean `CefShutdown` in logs.
- If the CEF archive or external tooling is missing, state that verification was not possible and mention the missing prerequisite.

### Smoke Test

Use `RunSmokeTest.ps1` for runtime/API checks. It builds PrismaUI and `test_plugin`, copies the distributions into Mod Organizer mods, launches Skyrim through SKSE, waits for the `PRISMAUI_SMOKE_TEST_PASS` marker in `PrismaUITest.log`, lets the test plugin request game exit, then checks the PrismaUI and PrismaUITest logs.

- Default debug smoke run:
  ```powershell
  .\RunSmokeTest.ps1
  ```
- Useful options: `-Preset release`, `-SkipBuild`, `-NoLaunch`, `-SmokeTimeoutSeconds <seconds>`, `-ExitTimeoutSeconds <seconds>`, and `-ForceExitOnTimeout`.
- The script copies PrismaUI to `{MO_dir}\mods\PrismaUI` and the test plugin to `{MO_dir}\mods\test_plugin`.
- Normal exit is automatic: the script sets `PRISMAUI_SMOKE_AUTO_EXIT=1`, and `test_plugin` sets `RE::Main::quitGame` only after the API smoke callbacks have passed.
- Logs checked/printed by the script:
  - CEF: `{MO_dir}\overwrite\PrismaUI\logs\cef.log`
  - PrismaUI: `%USERPROFILE%\Documents\My Games\Skyrim Special Edition\SKSE\PrismaUI.log`
  - test plugin: `%USERPROFILE%\Documents\My Games\Skyrim Special Edition\SKSE\PrismaUITest.log`
