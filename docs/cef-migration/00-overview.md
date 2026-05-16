# CEF Migration Overview

## Goal

Migrate PrismaUI from Ultralight to CEF while preserving the core public PrismaUI API where it maps cleanly to CEF:

- Keep compatibility for normal view lifecycle, rendering, input, JavaScript invocation, JS-to-native callbacks, console callbacks, clipboard, and IME where practical.
- Keep `PrismaView` as the public `uint64_t` handle unless a later API version has a strong reason to change it.
- Public API changes are allowed when CEF has a materially better model or when preserving Ultralight behavior would add fragile compatibility code.
- Inspector APIs are explicitly allowed to change or be removed in a new API version.
- Do not silently break existing vtable ordering. If API changes are needed, add a new interface version or make an intentional breaking release with clear documentation.

The internal rendering model changes from "one Ultralight view and one D3D texture per Prisma view" to "one CEF OSR browser compositing a main shell page, with one iframe per public Prisma view". The normal view stack is then rendered as one CEF texture over Skyrim. Debugging should align with Chromium DevTools: if all iframe views are visible from DevTools, the old per-view inspector surface does not need to be preserved.

## Target Architecture

```text
Skyrim Present hook
  -> original Present
  -> PrismaUI render pass
      -> process view operations
      -> pump/request CEF frame
      -> copy latest CEF OSR frame into app-owned D3D texture
      -> draw single CEF overlay texture
      -> draw Prisma cursor

CEF main browser
  -> transparent OSR browser at game backbuffer size
  -> loads internal Prisma shell document
  -> shell creates/removes/reorders/show/hides iframes
  -> each iframe represents one public PrismaView ID
```

## Migration Strategy

Use an adapter-style migration so code can move in controlled slices:

1. Establish a CEF baseline beside the current Ultralight code.
2. Add CEF dependency, helper process, runtime copying, and loading.
3. Introduce a `CefRuntime` and single transparent OSR browser.
4. Port GPU accelerated shared-texture rendering as the primary path, with CPU `OnPaint` as fallback only.
5. Add the iframe shell and map public view IDs to iframe frames.
6. Rewire `ViewManager` to the CEF backend while preserving normal view behavior.
7. Rebuild JS invocation and JS-to-C++ callbacks through CEF frame execution and process messages.
8. Convert input, clipboard, and IME event delivery to CEF APIs.
9. Replace the old inspector model with CEF DevTools and update or remove inspector API methods.
10. Remove Ultralight dependencies only after feature parity is verified.

## Important Constraints

- CEF is multi-process by default. The plugin should package and use a dedicated subprocess executable instead of relying on Skyrim.exe as the renderer subprocess.
- CEF UI APIs must be called on the CEF UI thread. Use `CefPostTask(TID_UI, ...)` or a narrow runtime wrapper.
- D3D11 texture creation, resource release, and drawing should stay on the render/present path.
- CEF frame rate and D3D frame rate are separate. The render loop draws the latest available CEF frame and should not block waiting for CEF.
- GPU rendering through CEF accelerated OSR shared textures is the preferred path. CPU BGRA `OnPaint` upload exists only as a compatibility fallback and should be clearly logged when active.
- Iframe-backed multiview means normal Prisma views share one browser texture. View order, visibility, and focus become shell DOM state.
- Log all important runtime steps and failures: CEF loading, CEF initialization, subprocess path, browser creation, shell load, iframe creation/destruction, GPU/CPU paint path selection, texture creation/recreation, JS bridge messages, input capture transitions, DevTools open/close, shutdown, and all CEF callback errors.

## Source References

- Current Ultralight runtime: `src/PrismaUI/Core.*`
- Current view lifecycle: `src/PrismaUI/ViewManager.*`
- Current bitmap upload and drawing: `src/PrismaUI/ViewRenderer.*`
- Current JS bridge: `src/PrismaUI/Communication.*`
- Current input and IME: `src/PrismaUI/InputHandler.*`, `src/PrismaUI/ImeHelper.*`
- Current inspector: `src/PrismaUI/Inspector.*`, `src/PrismaUI/Listeners.*`
- CEF proof of concept: `C:\work\TestCef`

## Open Decisions

- Whether to keep CEF message pumping on the present path or use `settings.multi_threaded_message_loop = true`. The safer plugin shape is to isolate CEF with its own UI thread, but the first spike should verify this inside Skyrim.
- Whether local views should stay as `file:///.../Data/PrismaUI/views/...` URLs or move to a custom `prisma://views/...` scheme. A custom scheme is cleaner long term, but file URLs are the closest match to current behavior.
- What the next public API version should expose for DevTools, if anything. The default direction is to expose a simple "open/show DevTools" capability instead of preserving `CreateInspectorView`, `SetInspectorBounds`, and separate inspector texture behavior.
