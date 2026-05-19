# Step 10 - Remove Ultralight

## Goal

Delete the Ultralight, AppCore, and JavaScriptCore backend after the CEF backend has reached parity and Step 9 has made the inspector API break explicit. This step is a clean cutover: after it lands, PrismaUI builds, packages, starts, renders, and shuts down without any Ultralight DLLs, headers, resources, or fallback paths.

## Preconditions

- Steps 3 through 9 are complete on the CEF path.
- Normal view create, destroy, show, hide, focus, unfocus, order, and active-focus queries are implemented through CEF iframe-backed views.
- Rendering uses accelerated CEF OSR shared textures by default, with CPU paint upload retained only as a logged compatibility fallback.
- JavaScript invoke, interop calls, JS listeners, console callbacks, input, clipboard, and IME are verified against CEF frames.
- The Step 9 DevTools replacement is implemented and documented; the old per-view Ultralight inspector API is not half-supported.
- Release configure and build succeed before removal so failures introduced by this step are attributable.

Do not start this step while the runtime still has a supported Ultralight fallback. Remove the fallback in the same cutover instead of leaving a dead backend behind.

## Edit Scope

- `CMakeLists.txt`
- `cmake/ExternalDependencies.cmake`
- `cmake/ultralight.cmake`
- `cmake/cef.cmake` only for package/copy adjustments needed by the new CEF-only layout
- `src/main.cpp`
- `src/Utils/DllLoader.h`
- `src/PrismaUI/Core.*`
- `src/PrismaUI/ViewManager.*`
- `src/PrismaUI/ViewRenderer.*`
- `src/PrismaUI/Communication.*`
- `src/PrismaUI/InputHandler.*`
- `src/PrismaUI/ImeHelper.*`
- `src/PrismaUI/Listeners.*`
- `src/PrismaUI/Inspector.*`
- `src/API/*`
- `src/PrismaUI_API.h`
- `assets/`, `README.md`, `NOTICES.txt`, and distribution copy rules
- `external/ultralight-free-sdk-1.4.1-dev-win-x64.7z` only after build/package references are gone

## Removal Order

1. Freeze the public ABI shape from Step 9:
   - keep `PrismaView` as `uint64_t`;
   - keep the source-level `IVPrismaUI1`, `IVPrismaUI2`, and `IVPrismaUI3` names;
   - keep the Step 9 numeric epoch values `V1 = 4`, `V2 = 5`, and `V3 = 6`;
   - keep `OpenDevTools`, `CloseDevTools`, and `IsDevToolsOpen` in the intended `IVPrismaUI1` vtable position;
   - keep legacy numeric requests `0`, `1`, and `2` returning `nullptr`.
2. Remove Ultralight runtime entry points from startup and shutdown:
   - delete `DllLoader::LoadUltralightLibraries()` calls from `SKSEPlugin_Load`;
   - remove direct references to `AppCore`, `Ultralight`, `UltralightCore`, `WebCore`, and JavaScriptCore from startup logs and error paths;
   - ensure CEF initialization, browser close, DevTools close, and `CefShutdown` are the only browser-engine lifecycle paths.
3. Remove Ultralight ownership from core state:
   - delete `ultralight::Renderer`, `ultralight::View`, `ultralight::BitmapSurface`, listener, and JS context fields;
   - remove the Ultralight executor if no non-CEF work still uses it;
   - keep D3D11 resource ownership on the render/present path;
   - keep CEF UI-thread work behind narrow `CefRuntime` wrappers.
4. Remove old view and render code:
   - delete per-view Ultralight creation, load, update, render, dirty-bounds, and bitmap-copy logic;
   - delete texture fields that only existed for per-view or inspector Ultralight surfaces;
   - keep the CEF shell browser texture and CPU fallback buffers;
   - keep view ordering, visibility, and focus as shell DOM state.
5. Remove JavaScriptCore and Ultralight listener code:
   - replace any remaining `JSContextRef`, `JSValueRef`, `JSStringRef`, `ultralight::LoadListener`, and `ultralight::ViewListener` includes/usages with CEF frame execution or process-message handling;
   - delete obsolete listener classes once no callsite remains;
   - keep callback-state ABI behavior from V3 unchanged.
6. Remove the old inspector implementation:
   - delete `src/PrismaUI/Inspector.*` from the build;
   - delete inspector-specific texture allocation, draw calls, input routing, bounds, visibility, and focus fields;
   - retain only the CEF DevTools lifecycle from Step 9.
7. Reconcile input and key helpers:
   - keep reusable Win32 key conversion code only if it feeds correct `CefKeyEvent` values;
   - delete Ultralight key/event wrappers and names;
   - verify clipboard and IME code no longer injects script through an Ultralight context.
8. Remove build-system references:
   - remove `${ULTRALIGHT_INCLUDE_DIR}` from include directories;
   - remove `${ULTRALIGHT_LIBRARY_DIR}` and `target_link_directories` if it becomes empty;
   - remove `AppCore`, `Ultralight`, `UltralightCore`, and `WebCore` from link libraries;
   - remove `/DELAYLOAD:UltralightCore.dll`, `/DELAYLOAD:WebCore.dll`, `/DELAYLOAD:Ultralight.dll`, and `/DELAYLOAD:AppCore.dll`;
   - remove Ultralight post-build copy commands;
   - remove `include(ultralight)` from `cmake/ExternalDependencies.cmake`;
   - delete `cmake/ultralight.cmake` after no CMake file includes it.
9. Make packaging CEF-only:
   - copy `libcef.dll`, CEF helper binaries, CEF resource files, `locales`, snapshot/blob files if required by the selected CEF build, and `PrismaUICefSubprocess.exe`;
   - copy Prisma shell assets under `Data/PrismaUI` without Ultralight `resources` or `inspector` directories;
   - fail the configure or packaging step if a required CEF runtime file is missing;
   - ensure the packaged runtime does not contain Ultralight DLLs.
10. Update documentation and notices:
   - remove install/runtime instructions that require the Ultralight archive or `Data/PrismaUI/libs` Ultralight DLLs;
   - document CEF archive and subprocess requirements;
   - replace Ultralight notice text with CEF/Chromium license and notices required for redistributed CEF binaries;
   - document known CEF differences from Ultralight before release.
11. Search and delete stragglers:
   - `ultralight`, `Ultralight`, `AppCore`, `WebCore`, `JavaScriptCore`, `BitmapSurface`, `ViewConfig`, `JSContextRef`, `JSValueRef`, `JSStringRef`, `CreateInspectorView`, `SetInspectorVisibility`, `SetInspectorBounds`, `IsInspectorVisible`;
   - every remaining hit must be either removed or intentionally retained in historical migration documentation.

## Build And Packaging Tasks

1. Configure from a clean release preset after removing `include(ultralight)`.
2. Build `PrismaUI.dll` and `PrismaUICefSubprocess.exe`.
3. Inspect the generated `dist/PrismaUI_<version>/PrismaUI/libs` package contents:
   - CEF runtime files are present;
   - `PrismaUICefSubprocess.exe` is present;
   - no `AppCore.dll`, `Ultralight.dll`, `UltralightCore.dll`, or `WebCore.dll` is present.
4. Inspect `dist/PrismaUI_<version>/PrismaUI`:
   - shell assets are present;
   - CEF resources/locales are present in the runtime location expected by `CefRuntime`;
   - old Ultralight `resources` and `inspector` folders are absent unless a CEF file explicitly requires the same directory name.
5. Confirm `external/ultralight-free-sdk-1.4.1-dev-win-x64.7z` is not referenced by CMake, scripts, README, or packaging docs.

## Runtime Smoke Test

1. Start Skyrim with the CEF-only package.
2. Confirm logs show:
   - CEF dependency paths;
   - CEF initialization success;
   - subprocess path;
   - shell browser creation;
   - selected GPU or CPU paint path;
   - no Ultralight load, renderer, resource, inspector, or shutdown messages.
3. Create a local HTML view and confirm it renders transparently over the game.
4. Focus the view and verify mouse, wheel, keyboard, text input, clipboard, and IME behavior.
5. Create multiple views, change order, hide/show, and destroy while the shell remains alive.
6. Open DevTools and inspect at least one `prisma-view-<PrismaView>` iframe.
7. Exit the game and confirm browser close, DevTools close if open, and `CefShutdown` complete without hanging.

## Acceptance Criteria

- No production source or CMake file includes or links Ultralight, AppCore, WebCore, or JavaScriptCore.
- No production source uses Ultralight types, JavaScriptCore types, Ultralight listeners, or `BitmapSurface`.
- `cmake/ultralight.cmake` is deleted or unreachable from the build.
- Release configure and build succeed from the documented preset commands.
- The distribution package contains the CEF runtime, subprocess executable, shell assets, and required notices.
- The distribution package does not contain Ultralight DLLs, Ultralight resources, or the old Ultralight inspector.
- Runtime startup, rendering, DevTools, and shutdown logs mention CEF and do not mention Ultralight except in versioned migration documentation.
- Legacy inspector methods are absent from the public header, and Step 9 DevTools methods remain available.
- In-game smoke testing confirms normal view lifecycle, rendering, input, JS bridge, DevTools, and shutdown on the CEF-only package.

## Risks

- CEF/Chromium notice and license packaging is broader than Ultralight's and must be correct before redistribution.
- Removing `WinKeyHandler` too aggressively can regress keyboard text, shortcut, or IME behavior. Keep only the conversion code that directly supports `CefKeyEvent` correctness.
- CEF resource placement is less forgiving than Ultralight's. A package that builds can still fail at runtime if `.pak`, blob, locale, or subprocess files are copied to the wrong directory.
- Dead Ultralight fallback code can mask CEF failures during testing. Prefer deletion over compile-time-disabled remnants once this step starts.