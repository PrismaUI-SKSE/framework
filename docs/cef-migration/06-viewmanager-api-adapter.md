# Step 6 - ViewManager API Adapter

## Goal

Rewire PrismaUI's internal view lifecycle to CEF while preserving normal view behavior. Inspector-specific public API can change in a new version because Chromium DevTools is a better fit than emulating Ultralight's inspector view.

## Edit Scope

- `src/PrismaUI/Core.*`
- `src/PrismaUI/ViewManager.*`
- `src/PrismaUI/ViewOperationQueue.*` if backend operation semantics need adjustment
- new CEF view state structs
- `src/PrismaUI_API.h` and `src/API/API.h` only if a new API version is added for DevTools or old inspector methods are intentionally removed

## Tasks

1. Replace Ultralight fields in `Core::PrismaView` with backend-neutral or CEF fields:
   - remove `RefPtr<View> ultralightView`
   - keep public-facing state fields
   - add iframe/frame identifiers
   - add CEF load/focus flags
2. Keep `Core::PrismaViewId` and the `views` map unchanged for callers.
3. Preserve lazy initialization:
   - first `ViewManager::Create()` still initializes core
   - CEF browser creation may wait until HWND/D3D state exists
4. Update `Create`:
   - generate Prisma ID
   - resolve URL
   - store callback/state
   - enqueue shell iframe creation
5. Update `Show` and `Hide`:
   - update native state
   - send shell visibility command
   - if hiding focused view, preserve existing unfocus and pause cleanup behavior
6. Update `Focus` and `Unfocus`:
   - preserve FocusMenu, pause, and control-map behavior
   - set current focused Prisma view in `InputHandler`
   - send shell focus/blur command
   - focus the CEF browser host if needed
7. Update `HasFocus` and `HasAnyActiveFocus`:
   - return from native state, not CEF frame objects
8. Update `SetOrder` and `GetOrder`:
   - update native order
   - send shell `z-index` update
9. Update `Destroy`:
   - clear pending operations
   - unfocus if focused
   - remove JS callbacks
   - remove iframe
   - clear view state
10. Log public view lifecycle state changes:
   - create request and resolved URL
   - show/hide
   - focus/unfocus, including game pause/control-map changes
   - order changes
   - destroy start/completion
   - operation queue failures or attempts to operate on missing views

## Acceptance Criteria

- Existing normal view API methods compile with no signature changes unless a broader intentional API break is chosen.
- Existing mod-facing behavior remains the same for normal view lifecycle operations.
- Inspector methods are either moved to a new DevTools-oriented API, deprecated, or removed with release notes.
- Existing interface vtable order is not accidentally changed. If compatibility is retained, add new methods only through a new interface version.
- Logs identify the public Prisma view ID for all lifecycle operations and errors.

## Risks

- The current code often uses Ultralight object existence as "view ready". CEF needs explicit ready states because the iframe frame may appear after shell command execution.
- Focus state should be owned by PrismaUI native state, not inferred from CEF DOM focus, otherwise hidden iframes and game focus menus can diverge.
