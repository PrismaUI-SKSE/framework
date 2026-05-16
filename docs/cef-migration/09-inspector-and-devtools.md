# Step 9 - DevTools And Inspector API Break

## Goal

Replace the Ultralight-style inspector view API with a CEF DevTools model. The old per-view inspector surface does not need to be preserved if Chromium DevTools can inspect the main browser and its iframe-backed Prisma views.

## Direction

Treat this as an intentional public API break or a new API-version boundary:

- Do not spend migration effort emulating `CreateInspectorView`, `SetInspectorVisibility`, `IsInspectorVisible`, and `SetInspectorBounds` unless there is a real consumer requirement.
- Prefer one DevTools entry point for the main CEF browser.
- Use Chromium's frame tree to inspect individual Prisma iframe views.
- Document that all Prisma views are represented as iframes in the DevTools target.

## Recommended API Shape

If a public debug API is still needed, replace the old inspector methods with a simpler DevTools-oriented surface in a new interface version:

```cpp
virtual void OpenDevTools() noexcept = 0;
virtual void CloseDevTools() noexcept = 0;
virtual bool IsDevToolsOpen() noexcept = 0;
```

Optional later additions:

- `OpenDevToolsForView(PrismaView view)` if CEF frame targeting proves reliable.

## Implementation

Use standard Chromium DevTools only:

1. Enable CEF remote debugging or call `ShowDevTools` with a normal CEF window if viable in the Skyrim process.
2. Let Chromium DevTools inspect the main browser.
3. Rely on iframe visibility in DevTools to debug individual Prisma views.
4. Remove the old inspector texture and input routing code.
5. Log DevTools open/close requests, success/failure, target browser ID, and remote debugging port if remote debugging is enabled.

Do not implement embedded OSR DevTools. It would recreate a second rendering/input stack for debug-only UI and does not fit the iframe-based CEF architecture.

## Migration Tasks

1. Mark existing inspector methods as deprecated or remove them in a new API version.
2. Remove `src/PrismaUI/Inspector.*` from the critical normal rendering path.
3. Remove inspector texture fields from `Core::PrismaView`.
4. Remove special inspector input routing from `InputHandler`.
5. Add DevTools lifecycle methods to the CEF runtime.
6. Document how to inspect a specific Prisma view iframe in DevTools.

## Acceptance Criteria

- Normal Prisma view rendering and input do not depend on inspector resources.
- DevTools can inspect the main CEF browser and show iframe-backed Prisma views.
- Old inspector API behavior is not accidentally half-supported.
- Any public API break is explicit in headers, README, and migration notes.
- Logs show how DevTools was opened and which CEF browser it targets.

## Risks

- Standard DevTools may be less convenient in-game than an embedded overlay, but it aligns better with CEF and keeps the core runtime simpler.
- Normal-window DevTools may need focus/window-management testing inside Skyrim's process.
