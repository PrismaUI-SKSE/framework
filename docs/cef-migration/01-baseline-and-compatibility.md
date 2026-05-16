# Step 1 - Baseline And Compatibility Matrix

## Goal

Document the current Ultralight behavior before replacing it. This gives every later CEF change a concrete compatibility target.

## Edit Scope

- Documentation only.
- Documentation only.
- Classify public APIs as "preserve", "adapt", or "break/change" before implementation.
- No Ultralight removal yet.

## Tasks

1. Create a behavior matrix for each public method in `src/PrismaUI_API.h` and `src/API/API.h`.
   - Mark normal view APIs as compatibility targets unless CEF makes a method unusually expensive or fragile.
   - Mark Inspector APIs as migration candidates rather than compatibility requirements.
2. Record current threading expectations:
   - Public API can be called from mod code.
   - View operations are queued with `ViewOperationQueue`.
   - Ultralight work runs on `Core::ultralightThread`.
   - D3D work runs in `Core::D3DPresent`.
3. Record current URL behavior:
   - `http://` and `https://` are passed through.
   - Other paths become `file:///views/<htmlPath>` relative to `Data/PrismaUI`.
4. Record current per-view state:
   - hidden/visible
   - focused/unfocused
   - paused game flag
   - order
   - scroll pixel size
   - loading finished
   - DOM ready callback
   - console callback
   - registered JS listeners
5. Record current rendering behavior:
   - each view is full screen and transparent
   - lower order draws first
   - higher order appears on top
   - cursor draws last
6. Record current input behavior:
   - only focused view receives input
   - mouse move/down/up, scroll, key down/up, char input, clipboard shortcuts, and custom IME state are handled
7. Record current inspector behavior separately:
   - current inspector has its own bounds, visibility, texture, and input routing
   - CEF migration can replace this with Chromium DevTools over the main browser where all view iframes are inspectable
   - old inspector methods may be deprecated, removed, or changed in a new API version

## Acceptance Criteria

- A checklist exists that maps every public API method to expected CEF behavior or an intentional breaking-change decision.
- Known behavior differences are explicitly listed before implementation begins.
- No implementation work starts until the iframe approach is validated against API methods that need per-view JS execution.

## Risks

- `CreateView` currently creates independent page contexts. Iframes share one top-level browser process and texture. The shell must make this invisible to API users.
- Remote `http` or `https` views may have different iframe security behavior. C++ can execute script in a CEF frame, but DOM access from the shell page may be blocked by normal same-origin rules.
- Inspector behavior should not drive rendering architecture. If Chromium DevTools can inspect the iframe tree, prefer that over emulating Ultralight's local inspector surface.
