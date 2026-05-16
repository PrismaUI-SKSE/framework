# PrismaUI Agent Notes

This repository is an SKSE plugin for Skyrim that exposes a C API for mods to render HTML/CSS/JS UI through Ultralight. The core implementation is C++23, CommonLibSSE-NG, D3D11, DirectXTK `SpriteBatch`, JavaScriptCore, and Ultralight 1.4.1-dev.

## Working Rules

- Keep changes scoped. This plugin runs inside Skyrim's process, so avoid broad refactors unless the task explicitly calls for them.
- Preserve the public ABI in `src/PrismaUI_API.h` and the matching vtable method order in `src/API/API.h`.
- Treat Ultralight objects as UI-thread-owned. Use `Core::ultralightThread` for Ultralight `View`, JS context, load/listener, and renderer operations.
- Treat D3D11 texture/resource work as render-thread work. Texture creation, mapping, drawing, and release are handled from the present/render path.
- Do not directly touch `external/commonlibsse-ng` unless the task is explicitly about vendored CommonLibSSE.
- Prefer `rg` for searches. The `external/` tree is large; scope searches to `src`, `cmake`, `assets`, and docs unless dependency code is relevant.
- Use existing logging style via `logger::info/warn/error/debug/critical`.
- Follow `.clang-format`: Google base style, 4-space indents, no tabs, 120-column limit.
- Default to ASCII for new code/docs unless updating text that already uses non-ASCII.

## Build And Packaging

- Use direct CMake preset commands from a VS 2022 Developer PowerShell or Developer Command Prompt.
- Release configure/build:
  - `cmake -S . --preset=release`
  - `cmake --build --preset=release --parallel 8`
- Debug configure/build:
  - `cmake -S . --preset=debug`
  - `cmake --build --preset=debug --parallel 8`
- `BuildRelease.ps1` exists as a convenience wrapper, but do not use it as the default build instruction.
- Requires `VCPKG_ROOT`, Ninja, VS 2022 C++ tooling, and `external/ultralight-free-sdk-1.4.1-dev-win-x64.7z`.
- CMake extracts Ultralight to `build/external_builds/ultralight`.
- Build output: `build/<preset>/bin/PrismaUI.dll`.
- Distribution output: `dist/PrismaUI_<version>/`, including `PrismaUI/libs`, `PrismaUI/resources`, `PrismaUI/inspector`, and `SKSE/plugins/PrismaUI.dll`.
- `UpdateExternalDeps.ps1` prepares submodules and warns if the Ultralight archive is missing. It can remove/reinitialize submodule folders, so do not run it casually in a dirty tree.

## Repository Map

- `src/main.cpp`: SKSE entry point, logger setup, Ultralight DLL loading, SKSE messaging, API export.
- `src/PrismaUI_API.h`: public modder-facing API. Mods may copy this header.
- `src/API/`: implementation of the exported PrismaUI API singleton.
- `src/PrismaUI/Core.*`: global runtime state, D3D present hook integration, Ultralight renderer lifecycle, render loop, shutdown.
- `src/PrismaUI/ViewManager.*`: view lifecycle, show/hide/focus/unfocus, order, destroy, console callbacks.
- `src/PrismaUI/ViewRenderer.*`: bitmap-to-buffer copy, D3D texture upload, view compositing, cursor draw.
- `src/PrismaUI/InputHandler.*`: Win32 subclass hook, Skyrim input sink, mouse/key/scroll queueing, clipboard, IME focus tracking.
- `src/PrismaUI/ImeHelper.*`: Windows IME context management and custom JS-dispatched composition/candidate state.
- `src/PrismaUI/Communication.*`: JS eval, native-to-JS calls, JS-to-C++ callback binding.
- `src/PrismaUI/Listeners.*`: Ultralight load/view listeners, DOM-ready and console callback dispatch, inspector creation callback.
- `src/PrismaUI/Inspector.*`: local Ultralight inspector lifecycle and rendering.
- `src/PrismaUI/ViewOperationQueue.*`: per-view operation queues processed from the present loop.
- `src/Hooks/`: trampoline install wrappers for D3D hooks.
- `src/Menus/FocusMenu/`: hidden Scaleform menu used to capture UI focus and cursor behavior.
- `src/Menus/CursorMenu/`: hook that hides vanilla cursor menu while PrismaUI has active focus.
- `src/Utils/`: DLL loader, encoding helpers, NanoID, priority single-thread executor, key conversion.
- `assets/`: files copied into `Data/PrismaUI` distribution, currently includes `misc/cursor.png`.
- `cmake/`: CommonLibSSE, Ultralight, dependency, and compiler flag setup.

## Runtime Architecture

### Plugin Load

`SKSEPlugin_Load` in `src/main.cpp`:

- initializes SKSE and logging
- loads Ultralight DLLs from `Data/PrismaUI/libs` before any Ultralight API use
- registers `SKSEMessageHandler`
- allocates trampoline storage

On `kDataLoaded`, `CursorMenuEx::InstallHook()` hooks Skyrim's cursor menu.

`RequestPluginAPI` returns one of `IVPrismaUI1`, `IVPrismaUI2`, or `IVPrismaUI3` implemented by `PluginAPI::PrismaUIInterface`.

### Core Initialization

The first `ViewManager::Create()` lazily initializes the core:

- installs the D3D present hook
- configures Ultralight platform, logger, font loader, file system, and resource path on `Core::ultralightThread`
- creates global `ultralight::Renderer`
- registers `FocusMenu`

Graphics state is acquired lazily in `Core::InitGraphics()` from `RE::BSGraphics::Renderer`.

### Frame Loop

`Core::D3DPresent()` calls the original present function first, then:

- initializes graphics if needed
- releases pending D3D resources
- processes one queued operation per view
- runs Ultralight work on `ultralightThread`
- creates pending Ultralight views
- processes input events
- calls `renderer->Update()`, `RefreshDisplay(0)`, and `Render()`
- copies dirty Ultralight bitmap surfaces into CPU buffers
- uploads ready buffers to D3D textures on the render thread
- draws all visible views
- draws the Prisma cursor last

### View Rendering And Mixing

Each `PrismaView` is a full-screen transparent Ultralight bitmap view:

- `ViewConfig::is_accelerated = false`
- `ViewConfig::is_transparent = true`
- `ViewConfig::enable_compositor = false`
- dimensions are `screenSize.width` by `screenSize.height`

The plugin does not composite multiple views inside Ultralight. Each view has its own bitmap buffer, D3D11 texture, and shader resource view. `ViewRenderer::DrawViews()` gathers visible views with a valid `textureView`, sorts by `PrismaView::order`, begins `SpriteBatch` with `commonStates->AlphaBlend()`, then draws each texture at `(0, 0)`.

Ordering rule:

- lower `order` draws earlier and appears underneath
- higher `order` draws later and appears on top
- new views default to `max(existing order) + 1`
- mods can change order through `SetOrder`

There is no C++ per-view rectangle, transform, clipping, or layout system for normal views. Positioning is handled by each view's HTML/CSS inside a full-screen transparent page.

### Thread Ownership

- Ultralight renderer/view operations must run on `Core::ultralightThread`.
- Public API callbacks back into mods are scheduled with `SKSE::GetTaskInterface()->AddTask` or `AddUITask`.
- Input events are collected from Win32/Skyrim callbacks, queued under mutex, and fired into the focused Ultralight view from the Ultralight thread.
- D3D11 textures and DirectXTK drawing are handled from the D3D present/render path.
- Shared view state is guarded by `Core::viewsMutex`; per-view pixel buffers and operation queues have their own mutexes.

## Public API Notes

- `PrismaView` is a `uint64_t` ID, not a pointer.
- V1 API: create/destroy, invoke JS, interop call, JS listener registration, focus, visibility, scroll size, order, inspector, active-focus query.
- V2 adds `RegisterConsoleCallback`.
- V3 adds state-aware callback variants. The caller owns `callbackState`; PrismaUI stores and passes it back unchanged.
- `CreateView` accepts `http://` and `https://` URLs directly. Other paths become `file:///views/<htmlPath>` relative to the Ultralight platform filesystem rooted at `Data/PrismaUI`.
- `Invoke` evaluates a JS string and optionally returns the result string.
- `InteropCall` calls a named global JS function with one string argument and avoids script construction overhead.
- `RegisterJSListener` exposes a global JS function with the requested name; JS should call it with a string argument.
- API string inputs are validated as UTF-8 and converted from the system ANSI code page as fallback.

## Input And Focus

- Only the focused Prisma view receives input events.
- `Focus(view, pauseGame, disableFocusMenu)` focuses the Ultralight view, enables input capture, opens `FocusMenu` unless disabled, disables several Skyrim controls, and optionally increments `RE::UI::numPausesGame`.
- Focusing one view queues unfocus operations for any other focused views.
- `Unfocus`, `Hide`, and `Destroy` clean up capture and pause state.
- `FocusMenu` is a hidden Scaleform menu using cursor/modal flags to keep the game in menu-mode style input while PrismaUI is active.
- `CursorMenuEx` hides the vanilla cursor menu while any PrismaUI view has focus.

## IME And Clipboard

- `InputHandler` subclasses the game HWND with `SetWindowSubclass`.
- Keyboard input uses `WinKeyHandler` to generate Ultralight key events.
- Clipboard reads/writes use Unicode clipboard text, with hard safety limits.
- IME is intentionally custom: native IME windows are suppressed and IME state is dispatched to JS as a `prismaIME_state` custom event.
- UI authors are expected to render their own IME overlay from that event. See `IME_SUPPORT.md`.

## Inspector

- Inspector assets must exist at `Data/PrismaUI/inspector/Main.html`.
- `CreateInspectorView` uses Ultralight's local inspector facility.
- Inspector has independent pixel buffer and D3D texture fields on `PrismaView`.
- Inspector input routing is special: mouse/scroll events go to inspector when the cursor is inside its bounds; key events go to inspector if it is visible and focused.

## Common Pitfalls

- Do not call Ultralight `View` methods directly from arbitrary threads.
- Do not release or recreate D3D resources from the Ultralight thread.
- Be careful when adding fields to `PrismaView`; consider shutdown, destroy, pending resource release, and thread synchronization.
- Do not change `PrismaUI_API.h` method order or insert methods into earlier interfaces. Add new API only by extending a new interface version.
- `ViewRenderer::RenderSingleView()` only copies when the Ultralight surface has dirty bounds. If adding behavior that requires a fresh texture, make sure `newFrameReady` and dirty bounds semantics are understood.
- `ViewOperationQueue` executes at most one operation per view per present-loop pass. Long operations block the Ultralight executor and can cause stutter.
- `BuildRelease.ps1` assumes a VS Community path unless overridden by `Build_Config_Local.ps1`; prefer direct CMake commands.
- `Utils::GetBasePath()` uses `std::filesystem::current_path() / "Data" / "PrismaUI"`, which is correct for game runtime assumptions but not arbitrary process working directories.

## Verification

There is no dedicated test suite in this repository. For code changes:

- At minimum, run `cmake -S . --preset=release` and `cmake --build --preset=release --parallel 8` when the local environment has VS, vcpkg, Ninja, and the Ultralight archive.
- For debug/runtime-sensitive work, also build `debug`.
- For rendering/input/IME changes, static build success is not enough; verify in-game when possible.
- If the Ultralight archive or external tooling is missing, state that verification was not possible and mention the missing prerequisite.
