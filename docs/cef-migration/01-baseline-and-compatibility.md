# Step 1 - Baseline And Compatibility Matrix

## Goal

Document the current Ultralight behavior and the intended CEF compatibility contract before any runtime migration begins.
This document is the Step 1 gate: no CEF dependency, build change, initialization, rendering rewrite, or Ultralight removal
starts until this baseline is complete and reviewed.

## Scope And Non-Goals

- Documentation only.
- Public ABI and vtable order are baseline inputs, not edit targets.
- Normal view APIs are compatibility targets for the CEF migration.
- Inspector/debug APIs are migration candidates and may move to a Chromium DevTools model behind a future API-version boundary.
- Do not add CEF, create a subprocess executable, call `CefInitialize`, alter source behavior, or touch vendored dependencies in this unit.

## Current Baseline Summary

- Public API versions currently stop at `IVPrismaUI3`; `PluginAPI::PrismaUIInterface` implements that latest interface.
- `PrismaView` is a public `uint64_t` handle backed by native `Core::views` state.
- Normal views are full-screen transparent Ultralight bitmap views. Positioning and layout are authored in HTML/CSS.
- Lower view order draws first and appears underneath; higher order draws later and appears on top; the Prisma cursor draws last.
- Public callbacks into mods are scheduled through `SKSE::GetTaskInterface()->AddTask`.
- Public API wrappers defensively ignore null handles/strings/callbacks where applicable and JS string payloads use UTF-8 with ANSI fallback in the existing wrapper layer.

## URL Compatibility

Current view creation resolves URLs as follows:

- `http://` and `https://` inputs are passed through unchanged.
- All other inputs become `file:///views/<htmlPath>` and are resolved by Ultralight's platform filesystem rooted at `Data/PrismaUI`.

CEF contract:

- Preserve the same accepted inputs for normal views.
- Remote URLs must remain loadable as direct frame URLs.
- Local views should initially preserve equivalent `Data/PrismaUI/views/<htmlPath>` resolution. An absolute `file:///.../Data/PrismaUI/views/...` URL is acceptable if it is behavior-compatible for shipped content.
- A future custom scheme is allowed only if it preserves mod-facing path behavior or is introduced through an explicit API-version boundary.

## Thread Ownership Compatibility

- Current Ultralight renderer/view/JS operations run on `Core::ultralightThread`.
- Current view operations that mutate per-view state are commonly queued through `ViewOperationQueue` and processed from the present loop.
- Current D3D11 texture creation, upload, drawing, and release are render-thread/present-path work.
- CEF UI APIs must run on the CEF UI thread through a narrow runtime wrapper or `CefPostTask(TID_UI, ...)`.
- CEF rendering output may be produced by CEF asynchronously, but PrismaUI D3D resources must remain owned by the render path.
- Public API calls may still originate from arbitrary mod code; the API layer must remain defensive and must not require callers to know backend thread rules.

## Native State That Must Remain Authoritative

Native PrismaUI state remains the compatibility source of truth under CEF:

- view ID allocation and validity
- resolved URL and requested view identity
- visible/hidden state
- focused/unfocused state
- pause-game state and focus-menu/control-map side effects
- scroll pixel size
- view order
- DOM-ready/load-finished state
- registered JS listener callbacks
- console callbacks
- callback-state pointers owned by API callers

CEF DOM, frame, or browser objects may mirror this state, but public query results should not depend on transient iframe DOM focus or frame lifetime when native state can answer reliably.

## API Compatibility Matrix

Classification:

- `preserve`: public behavior should remain compatible.
- `adapt`: public contract should remain compatible, but implementation mechanics must change for CEF.
- `break/change`: acceptable intentional API break/replacement candidate, limited here to inspector/debug behavior.

| Vtable order | Method | Class | Current Ultralight behavior | Intended CEF behavior | State and threading expectation | Later acceptance test | Known CEF risk |
|---|---|---|---|---|---|---|---|
| V1.01 | `CreateView` | adapt | Null path returns `0`; first call lazily initializes Core; creates a native ID, resolves URL, stores callback, starts visible, assigns `max(order)+1`, and later creates the Ultralight view on the UI thread. | Keep signature, handle type, lazy runtime startup, URL rules, visible default, order default, and DOM-ready callback behavior; create an iframe-backed view in the CEF shell instead of a standalone Ultralight view. | Native `views` entry, URL, order, hidden flag, load callback; public call can come from mod code; CEF browser/frame creation must run on CEF UI thread. | Creating local and remote views returns nonzero IDs, loads the expected URL, fires DOM-ready once on the SKSE task queue, and assigns increasing order. | CEF iframe creation is asynchronous; frame may not exist when the public handle is returned. |
| V1.02 | `Invoke` | adapt | Null view/script is ignored; script is validated as UTF-8 or converted from ANSI; evaluates in the target Ultralight view; callback receives result string or empty string on missing view/errors through SKSE task scheduling. | Evaluate in the target iframe frame and preserve callback semantics without blocking render or caller threads. | Target view/frame, script string, optional callback; CEF execution on target frame's UI/render-process bridge; mod callback on SKSE task queue. | Simple expressions return string results; missing or destroyed views call the callback with an empty string when a callback was supplied. | CEF has no synchronous `EvaluateScript` equivalent; result bridge needs request IDs and timeout/error handling. |
| V1.03 | `InteropCall` | adapt | Null view/function/argument is ignored; argument is UTF-8 validated or ANSI-converted; locks target JS context and calls a global function with one string argument. | Call a global function inside the target iframe frame with the same one-string argument contract. | Target view/frame, function name, argument; CEF frame execution on UI/render bridge. | A global JS function in the selected view receives exactly the supplied string; missing function logs but does not crash. | Must execute in the view frame, not the shell frame; safe string encoding must avoid script injection. |
| V1.04 | `RegisterJSListener` | adapt | Null view/name/callback is ignored; stores callback by `(viewId, name)`; binds a global JS function in the view after load or immediately if already loaded; JS calls pass one string argument back through SKSE task scheduling. | Preserve dynamic registration and callback shape using renderer-process bindings and browser-process messages scoped by view ID and name. | Callback map, listener name, target view/frame; registration from mod thread, binding in CEF renderer context, callback delivery on SKSE task queue. | JS in one view calls the registered name and only that view's native callback receives the argument. | Renderer process cannot hold C++ function pointers; listener state must be synchronized across processes and rebound after reload. |
| V1.05 | `HasFocus` | preserve | Invalid view returns false; otherwise checks Ultralight focus on the UI thread and returns false on exceptions. | Return PrismaUI native focus state, mirrored to CEF frame focus as needed. | Native focused flag plus CEF focus mirror; public query must be safe from mod code. | Focused view reports true and all other valid views report false after focus changes. | DOM focus inside iframes can diverge from game input capture if treated as authoritative. |
| V1.06 | `Focus` | preserve | Invalid view returns false; valid view returns true after enqueue; operation rejects hidden/not-ready views, unfocuses other focused views, focuses Ultralight view, enables input capture, opens FocusMenu unless disabled, disables selected Skyrim controls, and optionally increments `numPausesGame`. | Preserve return behavior, single-focused-view invariant, input capture, FocusMenu, control-map, and pause semantics; additionally focus CEF browser and target iframe. | Native focus/pause state, input capture, FocusMenu, control map; mutation queued today and CEF focus posted to UI thread. | Focusing one visible ready view captures input, optionally pauses, disables controls, and unfocuses the prior view without double-closing FocusMenu. | CEF browser focus and iframe DOM focus may require retries after shell DOM changes. |
| V1.07 | `Unfocus` | preserve | Invalid view ignored; queued operation clears pause, disables input capture, clears IME state, unfocuses Ultralight view, closes FocusMenu, and restores controls when the view was focused or partially ready. | Preserve cleanup side effects and blur the CEF frame/browser mirror. | Native focus/pause state, IME state, input capture, FocusMenu, controls; queued mutation plus CEF UI blur. | Unfocus after a paused focus decrements pause once, releases capture, restores controls, and reports no active focus. | Native cleanup must not depend on a still-live CEF frame during destruction. |
| V1.08 | `Show` | preserve | Invalid view ignored; queued operation marks hidden state false; already visible is a no-op. | Preserve native visible state and send shell command to show iframe. | Native hidden flag; queued operation; shell DOM mutation on CEF UI thread. | Showing a hidden view makes it render and does not change order or focus. | Shell visibility can lag native state; queries should remain native. |
| V1.09 | `Hide` | preserve | Invalid view ignored; queued operation unfocuses first if focused, then marks hidden true; already hidden is a no-op. | Preserve hide and unfocus side effects; hide iframe in shell. | Native hidden/focus/pause state, input capture; queued operation; shell DOM mutation on CEF UI thread. | Hiding a focused view releases capture/pause and prevents further input delivery. | Hidden iframe may retain DOM focus unless explicitly blurred. |
| V1.10 | `IsHidden` | preserve | Null or missing view returns true; valid view returns native hidden flag. | Preserve native query behavior. | Native hidden flag under `viewsMutex`; public query can run from mod code. | Invalid handles return true; show/hide transitions update the query result. | None if native state remains authoritative. |
| V1.11 | `GetScrollingPixelSize` | preserve | Null handle returns `0`; nonzero missing view returns default `28`; valid view returns native pixel size. | Preserve query behavior and use value when translating wheel deltas to CEF. | Native `scrollingPixelSize`; query under lock; CEF input conversion reads native state. | Default, custom, invalid-null, and invalid-nonzero cases match current behavior. | CEF wheel units differ from Ultralight; scaling must be validated by behavior, not just value storage. |
| V1.12 | `SetScrollingPixelSize` | preserve | Null handle ignored; valid value updates native state; `<=0` stores fallback `16`; missing view logs and ignores. | Preserve setter behavior and apply value in CEF wheel event conversion. | Native `scrollingPixelSize`; public call under lock; input path consumes it later. | Setting positive and nonpositive values changes subsequent scroll distance as expected. | Wrong CEF delta conversion can make the stored pixel size observable but ineffective. |
| V1.13 | `IsValid` | preserve | Returns false for null or missing view and true while the native entry exists. | Preserve native validity based on PrismaUI handle map, not CEF frame presence. | Native `views` map under shared lock. | Returned handle is valid until destroy removes it; random/nonzero missing handle is invalid. | Async CEF frame creation/destruction must not make valid handles flicker. |
| V1.14 | `Destroy` | adapt | Null/missing view ignored; clears pending operations, unfocuses if needed, erases native entry, removes JS callbacks, releases Ultralight resources on UI thread, and releases D3D resources. | Preserve handle invalidation, callback cleanup, focus cleanup, and render-resource cleanup; remove iframe and clear CEF frame/browser state. | Native map/callbacks, focus/pause state, pixel/texture resources; CEF cleanup on UI thread, D3D release on render path. | Destroyed view becomes invalid, no longer renders, no longer receives callbacks/input, and can be called safely twice. | CEF frame destruction is async; pending JS results/messages must be ignored after native destroy. |
| V1.15 | `SetOrder` | preserve | Null handle ignored; valid view updates native order; missing view logs and ignores. | Preserve native order and send shell `z-index` update. | Native `order`; shell DOM mirror on CEF UI thread. | Three visible views stack according to order after changes. | One shared CEF texture means ordering is DOM/CSS state; shell command failures affect visual result. |
| V1.16 | `GetOrder` | preserve | Null or missing view returns `-1`; valid view returns native order. | Preserve native query behavior. | Native `order` under shared lock. | Default increasing order and explicit order values are observable through the query. | None if native state remains authoritative. |
| V1.17 | `CreateInspectorView` | break/change | Requires bundled Ultralight inspector assets and a ready Ultralight view; requests a local inspector view on the Ultralight thread; no public handle is returned. | Do not emulate an embedded inspector surface by default; replace with Chromium DevTools in a new API version or documented break. | Current inspector view and assets are per-view Ultralight state; future DevTools state should belong to CEF runtime/debug API. | Later migration explicitly removes/deprecates this behavior or maps it to a documented DevTools entry point. | Half-supporting this would recreate a second rendering/input stack for debug-only UI. |
| V1.18 | `SetInspectorVisibility` | break/change | Missing view ignored; visible request may create inspector; stores inspector-visible flag, resets hover state, focuses inspector, and unfocuses the normal Ultralight view. | Replace with DevTools open/close or remove at an API-version boundary; do not require normal rendering to depend on inspector visibility. | Current inspector visibility/focus state and per-view inspector resources; future DevTools state should be separate from view compositing. | Visibility calls are either intentionally unsupported with documented behavior or replaced by DevTools behavior in the chosen API. | Embedded DevTools/inspector focus would interfere with game input capture and iframe focus. |
| V1.19 | `IsInspectorVisible` | break/change | Missing view returns false; valid view returns native inspector-visible flag. | Replace with DevTools-open query if a new debug API is added, otherwise document removal/deprecation. | Current inspector-visible flag; future DevTools runtime state. | Query behavior is explicitly specified for the new debug model or removed with release notes. | Maintaining the old per-view flag may imply false compatibility if DevTools is browser-wide. |
| V1.20 | `SetInspectorBounds` | break/change | Clamps minimum size to 32x32, clamps position to screen, stores inspector bounds, and resizes inspector view on Ultralight thread. | Replace with normal Chromium DevTools window management or omit; do not build CEF architecture around embedded inspector bounds. | Current per-view inspector bounds and texture size; future external DevTools window state if supported. | Bounds calls are intentionally unsupported/deprecated or mapped to documented DevTools window behavior. | OSR DevTools would need separate input/render paths and should be avoided. |
| V1.21 | `HasAnyActiveFocus` | preserve | Checks all Ultralight views on UI thread and returns true if any has focus; false for no views or exceptions. | Return native active-focus state across normal Prisma views. | Native focus state; public query safe from mod code. | True when any normal view owns PrismaUI input capture; false after all are unfocused/hidden/destroyed. | Inspector/DevTools focus must not make this query lie about normal view capture unless explicitly specified. |
| V2.01 | `RegisterConsoleCallback` | adapt | Null view ignored; non-null callback wraps messages and schedules mod callback on SKSE task queue; null callback unregisters for the view. | Preserve register/unregister behavior using CEF console-message callbacks scoped to the target iframe/view. | Per-view console callback; CEF browser/client console event to native map; delivery on SKSE task queue. | Console log/warn/error/debug/info from one view reports the expected view ID, level, and message; unregister stops delivery. | CEF console callbacks may originate from subframes with different source metadata; must map frame to Prisma view ID. |
| V3.01 | `CreateViewV2` | adapt | Same creation behavior as the v1 method, but stores and passes caller-owned callback state to the DOM-ready callback unchanged. | Preserve state-aware callback semantics while creating iframe-backed views. | Same state as creation plus opaque caller pointer; callback delivery on SKSE task queue. | Callback receives the created view ID and exactly the original state pointer. | Async frame creation must not outlive destroyed caller-owned state beyond current contract expectations. |
| V3.02 | `InvokeV2` | adapt | Same script behavior as the v1 method, but result callback receives caller-owned callback state unchanged. | Preserve state-aware result callback semantics through CEF result bridge. | Same state as invoke plus opaque caller pointer; callback delivery on SKSE task queue. | Result callback receives expected string and exactly the original state pointer. | Request-ID bridge must retain state only as long as current API semantics allow and drop safely on destroy. |
| V3.03 | `RegisterJSListenerV2` | adapt | Null callback ignored; otherwise same listener behavior as the v1 method, with caller-owned callback state passed back unchanged on each JS call. | Preserve state-aware JS listener semantics using CEF process messages. | Listener map plus opaque caller pointer; renderer binding and browser callback delivery. | JS callback receives argument and exactly the original state pointer for repeated calls. | Renderer-process rebinding after reload must not duplicate callbacks or lose state association. |
| V3.04 | `RegisterConsoleCallbackV2` | adapt | Null view or null callback ignored; unlike the v2 method, this path currently does not unregister on null; non-null callback receives state unchanged through SKSE task scheduling. | Preserve observed state-aware behavior unless an intentional API-version correction is chosen later. | Per-view console callback plus opaque caller pointer; CEF console event mapping; delivery on SKSE task queue. | Non-null callback receives view ID, level, message, and original state pointer; null callback behavior is explicitly tested against the chosen compatibility target. | CEF frame-to-view mapping and the v2/v3 null-callback difference can cause accidental behavior drift. |

## Iframe-Specific JavaScript Execution Risks

- CEF iframe-backed views share one top-level browser texture, but script execution must target the iframe frame for the public view ID.
- Shell-page DOM access to remote iframe content is limited by normal same-origin rules; native code should resolve the CEF frame and execute there instead of relying on shell DOM access.
- Listener bindings must be installed in the renderer process for each target frame and reinstalled after reloads.
- Result-returning script invocation needs an asynchronous bridge because CEF does not return evaluated values synchronously through `ExecuteJavaScript`.
- Destroyed views must ignore late renderer-process messages, pending invoke results, and delayed frame events.

## Inspector-To-DevTools Decision

The inspector rows are the intentional break/change candidates. CEF migration should prefer standard Chromium DevTools for the main CEF browser, where iframe-backed Prisma views are visible in the frame tree. The migration should not recreate Ultralight's per-view inspector texture, bounds, opacity, and special input routing unless a real consumer requirement overrides this decision. If public debug API remains necessary, add a new interface version with DevTools-oriented methods rather than inserting methods into existing interfaces.

## ABI And Vtable Guardrails

- Preserve `src/PrismaUI_API.h` method order for existing interfaces.
- Preserve `src/API/API.h` implementation declaration order so it continues to match the public interfaces.
- Do not insert methods into existing interface versions.
- Add new public API only by extending a new interface version unless an intentional breaking release is chosen and documented.
- Keep `PrismaView` as the public handle for normal views unless a future API version explicitly changes it.
- Treat this document as the compatibility checklist for future CEF implementation reviews.

## Known Differences To Validate Before Implementation Proceeds

- Independent Ultralight pages become iframe documents inside one CEF browser; tests must prove per-view isolation for JS, focus, visibility, ordering, console messages, and callbacks.
- CEF remote iframes may behave differently from direct top-level pages for focus, navigation, console reporting, and security.
- CEF script result handling is asynchronous and must match callback behavior without blocking the render path.
- CEF key, mouse, wheel, clipboard, and IME APIs differ from Ultralight events; game input capture behavior must remain native-owned.
- CEF DevTools is browser-wide; old inspector APIs should not be accidentally half-supported.

## Acceptance Criteria For Step 1

- The matrix above contains every public virtual method from `src/PrismaUI_API.h` and follows the declaration order implemented in `src/API/API.h`.
- Normal view APIs are documented as compatibility targets.
- Inspector/debug APIs are documented as break/change candidates toward DevTools or a future API-version boundary.
- URL behavior, thread ownership, native authoritative state, iframe JS risks, ABI guardrails, and known differences are explicit.
- No implementation work starts until this baseline is complete.