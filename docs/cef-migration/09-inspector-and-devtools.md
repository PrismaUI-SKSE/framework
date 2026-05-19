# Step 9 - DevTools And Inspector API Break

## Goal

Replace the Ultralight-style inspector view API with a CEF DevTools model. The old per-view inspector surface does not need to be preserved if Chromium DevTools can inspect the main browser and its iframe-backed Prisma views.

## Direction

Treat this as an intentional public API break or a new API-version boundary:

- Do not spend migration effort emulating `CreateInspectorView`, `SetInspectorVisibility`, `IsInspectorVisible`, and `SetInspectorBounds` unless there is a real consumer requirement.
- Prefer one DevTools entry point for the main CEF browser.
- Use Chromium's frame tree to inspect individual Prisma iframe views.
- Document that all Prisma views are represented as iframes in the DevTools target.

## Final API Shape

Step 9 is an intentional ABI epoch break, not a compatible extension. `InterfaceVersion` values `0`, `1`, and `2` are legacy Ultralight-inspector ABI values and are rejected with `nullptr`. The supported source-level interfaces remain `IVPrismaUI1`, `IVPrismaUI2`, and `IVPrismaUI3`, but their numeric values are now `V1 = 4`, `V2 = 5`, and `V3 = 6`.

The old per-view inspector methods were removed from `IVPrismaUI1`:

```cpp
CreateInspectorView(PrismaView view)
SetInspectorVisibility(PrismaView view, bool visible)
IsInspectorVisible(PrismaView view)
SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY, unsigned int width, unsigned int height)
```

They were replaced in the same vtable position by browser-wide DevTools methods:

```cpp
virtual void OpenDevTools() noexcept = 0;
virtual void CloseDevTools() noexcept = 0;
virtual bool IsDevToolsOpen() noexcept = 0;
```

Optional later additions:

- `OpenDevToolsForView(PrismaView view)` if CEF frame targeting proves reliable.

## Implementation

Use standard Chromium DevTools only:

1. `CefRuntime::OpenDevTools()` calls `CefBrowserHost::ShowDevTools` for the single PrismaUI shell browser using a normal native popup window parented to the game HWND when available.
2. `CefRuntime::CloseDevTools()` calls `CefBrowserHost::CloseDevTools` on the shell browser host.
3. DevTools lifecycle is tracked in `CefRuntime`, not on `PrismaView`, because all public Prisma views are iframes inside the shell browser.
4. A dedicated DevTools `CefClient` tracks DevTools browser create/close callbacks so user-close updates `IsDevToolsOpen()`.
5. Remote debugging is not enabled by default in Step 9. Logs explicitly note that remote debugging is disabled, and log open/close requests, target shell browser IDs, tracked DevTools browser IDs, and failures.
6. The old inspector texture/resource path and `src/PrismaUI/Inspector.*` are removed from normal rendering.

Do not implement embedded OSR DevTools. It would recreate a second rendering/input stack for debug-only UI and does not fit the iframe-based CEF architecture.

## Inspecting A Prisma View

1. Request the recompiled API (`InterfaceVersion::V1`, `V2`, or `V3` from the current header) and call `OpenDevTools()`.
2. In Chromium DevTools, inspect the main PrismaUI shell browser.
3. Select the frame named `prisma-view-<PrismaView>` in the frame tree, or inspect the iframe element with `data-prisma-view-id="<PrismaView>"`.
4. Use that frame context for DOM, console, and script inspection for the corresponding Prisma view.

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
