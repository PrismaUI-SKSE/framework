# Step 11 - Verification And Rollout

## Goal

Verify the CEF backend in layers before using it as the default PrismaUI runtime.

## Build Verification

Run from a Visual Studio 2022 developer shell:

```powershell
cmake -S . --preset=release
cmake --build --preset=release --parallel 8
```

When making debug/runtime-sensitive changes, also run:

```powershell
cmake -S . --preset=debug
cmake --build --preset=debug --parallel 8
```

## Runtime Smoke Tests

1. Start Skyrim with PrismaUI installed.
2. Confirm CEF initializes once.
3. Confirm the subprocess executable launches.
4. Confirm `Data/PrismaUI/logs/cef.log` or the configured CEF log path is written.
5. Confirm PrismaUI logs show CEF dependency paths, initialization start/success, subprocess path, and browser creation.
6. Create one local HTML view.
7. Confirm transparent rendering over the game.
8. Confirm logs show the GPU accelerated shared-texture path is active.
9. Hide, show, focus, unfocus, and destroy the view.
10. Exit the game and confirm CEF shutdown completes without hanging and logs browser close plus `CefShutdown`.

## Feature Tests

1. Multiview:
   - create three views
   - set different orders
   - hide and show middle/top views
   - destroy one view while others remain visible
2. JavaScript:
   - `Invoke` returns a simple expression result
   - `InteropCall` reaches a global function in the correct iframe
   - `RegisterJSListener` receives JS calls from the correct iframe
   - console callback reports log, warning, error, info, and debug levels
3. Input:
   - mouse hover and click
   - wheel
   - keyboard navigation
   - text entry
   - copy, paste, select all
4. IME:
   - focus text input
   - composition state reaches JS as `prismaIME_state`
   - committed text reaches the input
   - unfocus clears IME state
5. Remote URL:
   - create an `https://` view
   - verify load callback, console callback, focus, and JS invocation behavior
6. Resize:
   - change resolution or window size if available
   - confirm browser and iframe dimensions update
7. Recovery:
   - reload a failing URL
   - destroy during load
   - create/destroy many views in a row
8. DevTools and ABI break:
   - request legacy numeric interface versions `0`, `1`, and `2`; each returns `nullptr`
   - request updated `InterfaceVersion::V1`, `V2`, and `V3`; each returns the expected interface pointer after recompilation
   - confirm the removed inspector methods cannot be called from code compiled against the updated header
   - call `IsDevToolsOpen()` before opening, after `OpenDevTools()`, after user-closing the DevTools window, and after `CloseDevTools()`
   - open DevTools for the CEF shell browser and confirm logs include the request, target shell browser ID, tracked DevTools browser ID, and that remote debugging is disabled
   - confirm all Prisma iframe views are visible in the frame tree as `prisma-view-<PrismaView>`
   - inspect DOM and console output for at least one iframe-backed view

## Performance Checks

- Compare D3D FPS and CEF paint FPS.
- Verify GPU accelerated paint is active by default.
- Verify CPU paint is logged as fallback only if accelerated paint is unavailable.
- Track texture recreations; they should happen on resize or format change, not every frame.
- Confirm the render hook never blocks waiting for CEF work.
- Watch memory use when creating and destroying many iframe views.

## Logging Checks

- Startup logs include CEF paths, settings, subprocess path, and initialization result.
- Browser logs include create request, `OnAfterCreated`, shell load finish/fail, and close lifecycle.
- Rendering logs identify GPU versus CPU path and texture recreate/failure events.
- View logs include public view ID, iframe name, URL, lifecycle changes, and failed operations.
- JS bridge logs include listener binding, invoke request IDs, callback dispatch, and process-message errors.
- Input logs include focus/capture transitions, WndProc hook status, clipboard failures, and IME association changes.
- Shutdown logs include DevTools close when open, browser close request, browser closed, and `CefShutdown` completion.

## Rollout Plan

1. Build and package only the CEF runtime.
2. Run the CEF-only release candidate through the smoke test matrix above.
3. Publish the ABI break and DevTools replacement notes with the release.
4. Keep rollback as a package/version rollback, not a hidden runtime fallback.

## Acceptance Criteria

- Release build succeeds.
- In-game smoke tests pass.
- GPU accelerated CEF OSR is the normal rendering path; CPU fallback is verified but not the expected path.
- Important lifecycle and error logs are present.
- The Step 9 ABI break is documented: legacy numeric interface requests `0..2` return `nullptr`, supported values are `V1 = 4`, `V2 = 5`, and `V3 = 6`.
- Inspector API changes are documented as an intentional migration break, and old per-view inspector methods are absent from the public header.
- New DevTools methods occupy the intended `IVPrismaUI1` vtable position after `GetOrder` and before `HasAnyActiveFocus`.
- Known CEF migration differences are documented before release.
