# Step 10 - Remove Ultralight

## Goal

Delete Ultralight-specific code and packaging after CEF reaches functional parity.

## Preconditions

- Normal view create/destroy/show/hide/focus/order works through CEF.
- Rendering works through accelerated CEF OSR as the preferred path, with CPU fallback only for degraded compatibility.
- JS invoke, interop call, JS listeners, console callbacks, input, clipboard, and IME are verified.
- Inspector API break or DevTools replacement is documented.
- Release build succeeds.

## Edit Scope

- `CMakeLists.txt`
- `cmake/ultralight.cmake`
- `cmake/ExternalDependencies.cmake`
- `src/Utils/DllLoader.h`
- `src/Utils/WinKeyHandler/*` if no longer needed
- all remaining Ultralight and JavaScriptCore includes
- old inspector texture/input code if DevTools replaces it
- distribution copy rules
- README and notices

## Tasks

1. Remove Ultralight includes:
   - `<AppCore/Platform.h>`
   - `<JavaScriptCore/...>`
   - `<Ultralight/...>`
2. Remove Ultralight link libraries:
   - `AppCore`
   - `Ultralight`
   - `UltralightCore`
   - `WebCore`
3. Remove Ultralight delay-load options.
4. Remove Ultralight extraction and copy rules.
5. Replace `DllLoader::LoadUltralightLibraries()` with CEF-specific loading or remove explicit loading if CEF delay-load is reliable.
6. Remove `cmake/ultralight.cmake` only after `ExternalDependencies.cmake` no longer includes it.
7. Update distribution layout:
- CEF libs
   - CEF resources
   - locales
   - subprocess exe
   - Prisma shell assets
8. Update license and notices:
   - remove Ultralight-specific runtime notes if no longer distributed
   - add CEF/Chromium notices and license requirements
9. Search for stragglers:
   - `rg -n "ultralight|Ultralight|JavaScriptCore|AppCore|WebCore|BitmapSurface|ViewConfig" src cmake CMakeLists.txt`

## Acceptance Criteria

- No Ultralight or JavaScriptCore headers are included.
- No Ultralight libraries are linked or copied.
- Release build succeeds.
- Runtime package starts with only CEF/Chromium dependencies.
- Logs no longer mention Ultralight during normal startup, rendering, or shutdown.

## Risks

- Notices and license packaging must be handled carefully because CEF/Chromium has a larger third-party notice surface.
- Removing `WinKeyHandler` too early may lose useful Win32 key mapping logic. Keep reusable mapping code if it helps produce correct `CefKeyEvent` values.
