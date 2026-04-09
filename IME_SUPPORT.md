# IME Setup For PrismaUI

PrismaUI exposes an IME state event to allow custom display of IME composition and candidate windows for non-English text input.

## The Setup Flow

1. listen for PrismaUI's IME event in the page
2. render a composition and candidate overlay from that state
3. disable app shortcuts while IME is active
4. pass the same IME state to both the input and overlay

## Step 1: Receive IME State

PrismaUI dispatches a `prismaIME_state` custom event into the page. Attach your listener on `window` (or `document`):

```js
window.addEventListener('prismaIME_state', (e) => {
  const state = e.detail || {};
  // store state in your app (e.detail is the payload)
});
```

The **payload is in `event.detail`** - use it as the full IME state object.

The event should be treated as a full snapshot, not a delta. Expect it to update whenever the visible IME state changes while the text field is active, including:

- composition start
- composition text changes
- candidate list open/change/close
- composition commit/end
- focus loss or other clear/reset cases

The payload shape your app should expect is:

```json
{
  "active": true,
  "composition": "とうきょう",
  "caret": 5,
  "candidates": ["とうきょう", "東京", "トウキョウ", "東京都"],
  "selectedIndex": 2
}
```

Field meanings:

- `active`: whether PrismaUI currently considers IME composition active
- `composition`: the current uncommitted composition string
- `caret`: zero-based caret position inside `composition`; use it to split the composition string and draw an inline caret
- `candidates`: the currently visible candidate page
- `selectedIndex`: zero-based index of the highlighted candidate within `candidates`; use `-1` when there is no valid selection

## Step 2: Render A Custom Overlay

Render the composition text, caret, and candidate list from the IME state. The overlay is **display-only**: PrismaUI owns IME logic (arrow keys, number keys, commit).

Place the overlay near the input (e.g. directly below it). Keep it simple:

- hide it when IME is inactive (or when there is no composition and no candidates; showing when `composition` or `candidates` exist even if `active` is false can avoid flicker)
- split the composition string at the caret
- draw candidates with a selected style based on `selectedIndex`

## Step 3: Disable App Shortcuts While IME Is Active

While the user is composing text, your app-level keyboard handlers should get out of the way.

This is separate from PrismaUI's internal IME handling. PrismaUI owns the native IME integration; your web app still needs to stop its own shortcuts from reacting while IME is active. Your text input may handle `Tab`, arrow keys, `Enter`, `Escape`, and other shortcuts; the app may also have global key handlers (e.g. Escape to close). All of those should be treated as IME-owned while composing.

If you do not gate those keys, the app can submit, close the UI, or move selection while the user is still converting text.

## Step 4: Wire It In The App

Your app shell should pass IME state into both places that need it:

- the text input, so it can suspend shortcut handling
- the overlay, so it can render composition UI

When the UI (or input) is closed and reopened, clear or re-initialize IME state (e.g. set to inactive, empty composition, no candidates). Otherwise the overlay can show stale composition or candidates from the previous session.
