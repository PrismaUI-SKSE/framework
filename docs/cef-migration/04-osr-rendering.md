# Step 4 - OSR Rendering And D3D Texture Flow

## Goal

Replace Ultralight bitmap extraction with CEF off-screen rendering while preserving the existing DirectX 11 overlay draw behavior. GPU accelerated OSR shared-texture rendering is the primary path; CPU `OnPaint` upload is a fallback only.

## Edit Scope

- `src/PrismaUI/ViewRenderer.*`
- `src/PrismaUI/Core.*`
- new CEF render handler/client files
- possible small D3D helper module

## Tasks

1. Implement a CEF OSR client:
   - `CefClient`
   - `CefRenderHandler`
   - `CefLifeSpanHandler`
   - `CefDisplayHandler`
   - `CefLoadHandler`
2. Implement required render handler methods:
   - `GetViewRect`
   - `GetScreenInfo`
   - `OnAcceleratedPaint` for the primary GPU shared-texture path
   - `OnPaint` for CPU fallback only
3. Primary GPU accelerated path:
   - receive `CefAcceleratedPaintInfo::shared_texture_handle`
   - open the shared resource using the Skyrim D3D11 device
   - copy into an app-owned overlay texture
   - create/update one SRV for the full CEF browser texture
   - log first successful accelerated frame
   - log shared texture dimensions and format when they change
4. Keep CPU fallback as a degraded mode:
   - copy BGRA pixels from `OnPaint` into a guarded buffer
   - upload on the render thread into a dynamic `DXGI_FORMAT_B8G8R8A8_UNORM` texture
   - log when CPU fallback becomes active
   - log if the runtime switches between GPU and CPU paths
5. Update `DrawViews()` semantics:
   - normal Prisma views no longer draw one texture each
   - draw one main CEF overlay SRV
   - draw cursor last
   - remove the normal-view dependency on inspector textures; DevTools handling is covered separately in Step 9
6. Handle resize:
   - when Skyrim screen size changes, call `browser->GetHost()->WasResized()`
   - recreate app-owned overlay texture on dimension or format changes
7. Request frames:
   - after D3D init and before draw, call or post `SendExternalBeginFrame()`
   - do not wait for a paint callback; draw the latest complete texture
8. Log rendering and D3D errors:
   - missing D3D device/context
   - failure to open CEF shared texture
   - overlay texture/SRV creation failure
   - texture resize/recreate
   - CPU texture map/unmap failure
   - unexpected empty paint buffers or zero dimensions

## Acceptance Criteria

- A transparent CEF shell renders over Skyrim with correct alpha blending.
- Accelerated shared texture path is the default and preferred rendering path.
- CPU `OnPaint` fallback works if accelerated paint is unavailable, and logs that fallback is active.
- D3D resources are released from the render path or under explicit synchronization.
- No per-normal-view D3D textures are needed after iframe shell rendering is active.
- Logs clearly show whether GPU or CPU rendering is active.

## Risks

- The sample currently copies accelerated frames inside the CEF callback. PrismaUI should move toward render-thread ownership or guard the immediate context with explicit synchronization.
- Shared texture format may differ from the current SpriteBatch expectations. Log dimensions and format whenever the texture is recreated.
