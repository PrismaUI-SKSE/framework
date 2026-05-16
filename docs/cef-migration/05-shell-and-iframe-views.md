# Step 5 - Shell Page And Iframe-Backed Views

## Goal

Implement multiview rendering inside one CEF browser by using one iframe per public Prisma view.

## Edit Scope

- new `assets/shell/` or `assets/cef-shell/`
- new native shell command methods in `CefRuntime`
- `ViewManager` integration later in Step 6

## Shell Responsibilities

The shell document owns DOM composition:

- create iframe for a Prisma view ID
- destroy iframe
- set iframe `src`
- show/hide iframe
- focus iframe
- blur iframe
- set `z-index` from Prisma order
- resize iframes to full viewport
- apply `pointer-events` according to focus/capture rules if needed

Suggested DOM shape:

```html
<div id="prisma-root">
  <iframe
    id="prisma-view-123"
    name="prisma-view-123"
    data-prisma-view-id="123"
    src="file:///..."
  ></iframe>
</div>
```

Each iframe should be styled as:

```css
position: absolute;
left: 0;
top: 0;
width: 100vw;
height: 100vh;
border: 0;
background: transparent;
```

## Native Responsibilities

1. Maintain a C++ map:
   - Prisma view ID
   - iframe name
   - requested URL
   - hidden state
   - focused state
   - order
   - load state
2. Execute shell commands in the main CEF frame:
   - `window.__prismaShell.createView({ id, url, order, hidden })`
   - `window.__prismaShell.destroyView(id)`
   - `window.__prismaShell.setHidden(id, hidden)`
   - `window.__prismaShell.setOrder(id, order)`
   - `window.__prismaShell.focusView(id)`
   - `window.__prismaShell.blurView(id)`
3. Resolve the CEF frame for a view:
   - use iframe `name = prisma-view-<id>`
   - use `browser->GetFrameByName(name)` for script execution
   - maintain a frame-id cache from load events if needed
4. Log shell/view operations:
   - shell document load start, finish, and failure
   - iframe creation with view ID and URL
   - iframe load start, finish, and failure
   - iframe destroy/show/hide/order/focus/blur commands
   - missing frame or shell command failures

## URL Compatibility

Preserve current `CreateView` inputs:

- `http://...` and `https://...` remain direct URLs.
- Other inputs resolve under `Data/PrismaUI/views`.
- The initial implementation can use absolute `file:///C:/.../Data/PrismaUI/views/<htmlPath>`.
- A later cleanup can register `prisma://views/<htmlPath>` if file URL quirks become a problem.

## Acceptance Criteria

- Creating three Prisma views creates three iframes inside one CEF browser.
- `SetOrder` updates visual stacking without extra D3D textures.
- `Hide` and `Show` map to iframe visibility.
- `Destroy` removes the iframe and clears native state.
- Full-screen transparent view assumptions stay compatible with existing PrismaUI content.
- Logs can correlate every public view ID with its iframe name and URL.

## Risks

- Same-origin rules limit what the shell page can directly inspect inside iframe DOMs. Native C++ should execute view scripts in the target CEF frame instead of relying on shell DOM access.
- Remote iframes may have different focus and keyboard behavior from local iframes. Test remote URL support explicitly because the public API currently allows it.
