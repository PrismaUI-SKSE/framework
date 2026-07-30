#pragma once
// PrismaVR.h — Public interface for VR integration.
// Does NOT include any Prisma headers. Self-contained API.

#include <cstdint>

namespace PrismaVR {

	// Core lifecycle (called from Core.cpp)
	void Initialize();      // Detect VR, load OpenVR interfaces
	void OnFrame();         // Per-frame update from D3DPresent
	void Shutdown();        // Cleanup overlays and state

	// Query
	bool IsVRActive();      // Whether VR runtime is available and running

	// Optional JS API for modders who want custom 3D placement
	void SetViewPosition(uint64_t viewId, float x, float y, float z);
	void SetViewScale(uint64_t viewId, float scale);
	void SetViewWorldLock(uint64_t viewId, bool locked);

} // namespace PrismaVR

// -----------------------------------------------------------------------------
// PRISMAVR_API — export when building PrismaUI itself, import for a consumer
// that links against this header. CMake defines PrismaUI_EXPORTS automatically
// for the shared-library target, so no build-system change is needed.
// (Consumers that resolve these entry points via GetProcAddress never include
// this header and are unaffected either way.)
// -----------------------------------------------------------------------------
#if defined(PrismaUI_EXPORTS) || defined(PRISMAVR_BUILDING_DLL)
#  define PRISMAVR_API __declspec(dllexport)
#else
#  define PRISMAVR_API __declspec(dllimport)
#endif

// -----------------------------------------------------------------------------
// External texture access (added by PrismaUI SteamVR fork).
// These C exports let a sister SKSE plugin retrieve the underlying Ultralight
// D3D11 render-target of a PrismaUI view, so the plugin can use the same
// texture on its own OpenVR overlay (e.g. attached to a player controller).
//
// LIFETIME: the returned texture is AddRef'd on the caller's behalf — the
// caller MUST call Release() on it when done, or it leaks. The reference keeps
// the COM object alive even if PrismaUI recreates the view's texture (e.g. on
// a resize), but a held pointer stops receiving new frames the moment that
// happens — re-query per frame rather than caching. For cross-thread pixel
// access, prefer PrismaVR_GetViewBitmap: the CPU path has no GPU-object
// lifetime to manage at all.
// -----------------------------------------------------------------------------
extern "C" PRISMAVR_API void*    PrismaVR_GetViewTexture(uint64_t viewId);
extern "C" PRISMAVR_API uint32_t PrismaVR_GetViewTextureWidth(uint64_t viewId);
extern "C" PRISMAVR_API uint32_t PrismaVR_GetViewTextureHeight(uint64_t viewId);

// CPU bitmap access — safer for cross-thread use than the GPU texture handle.
// PrismaUI's ViewRenderer already maintains a CPU pixelBuffer in every view
// (it copies from Ultralight's BitmapSurface each frame). These accessors
// snapshot that buffer into a caller-provided destination under the per-view
// bufferMutex. This avoids any D3D11 device-context interaction in the calling
// plugin, which is critical to avoid render-thread crashes when the caller
// runs on a different thread from Skyrim's render thread.
//
// Pass outPixels=nullptr to query dimensions only.
// Pixel layout is BGRA8 (Ultralight's standard).
// Returns the CURRENT buffer contents unconditionally (no new-frame gating —
// that is PrismaVR_GetViewBitmapIfNew's job). Returns false only if the view
// doesn't exist, hasn't produced a first frame yet (empty buffer), or the
// caller buffer is too small.
extern "C" PRISMAVR_API bool PrismaVR_GetViewBitmap(
    uint64_t  viewId,
    void*     outPixels,        // may be nullptr for size-query
    uint32_t  outBufSize,
    uint32_t* outWidth,
    uint32_t* outHeight,
    uint32_t* outStride);

// Opt the given view OUT of PrismaUI's automatic VR-overlay promotion.
// Useful when a sister plugin (us) wants to render the view's content on its
// own OpenVR overlay (e.g. controller-attached) and doesn't want PrismaUI to
// create a duplicate overlay that captures laser-mouse input.
// Pass enabled=false to exclude; pass enabled=true to re-enable.
// Excluded views still render normally (PrismaUI keeps updating the bitmap),
// PrismaUI just won't create/manage an OpenVR overlay for them.
//
// THREADING: safe to call from any thread. This only flips the view's atomic
// externalConsumer flag; any overlay PrismaUI had already created for the view
// is torn down by SyncOverlays on the render thread within a frame (no VR
// resources are touched from the caller's thread).
extern "C" PRISMAVR_API void PrismaVR_SetViewVREnabled(uint64_t viewId, bool enabled);

// Resize the underlying Ultralight View. Drops the previous render target,
// causes the next frame to render at the new size. Useful to shrink large
// default 1920x1080 views into something more appropriate for a small VR
// overlay panel (e.g. 512x384) — reduces CPU rendering cost and IPC bandwidth.
// Returns false if the view doesn't exist or the resize call fails.
extern "C" PRISMAVR_API bool PrismaVR_ResizeView(uint64_t viewId, uint32_t width, uint32_t height);

// View enumeration — lets a sister plugin discover ALL currently-tracked
// PrismaUI views (from any consumer mod) and pick which one to display
// on its own OpenVR overlay. This is what turns the bridge from "one view
// hard-coded by the bridge plugin" into a universal VR adapter: any
// PrismaUI mod's view can be substituted into the slot.

// Returns the total number of currently-tracked views.
extern "C" PRISMAVR_API uint32_t PrismaVR_GetViewCount();

// Get info about the i-th view (sorted by view id, stable iteration if the
// view set doesn't change between calls). pathBuffer is filled with the view's
// originalUrl as a null-terminated UTF-8 string. If pathBufferSize is too small,
// the string is truncated with a trailing \0.
// Returns true on success, false if index is out of range.
extern "C" PRISMAVR_API bool PrismaVR_GetViewInfo(
    uint32_t  index,
    uint64_t* outId,
    char*     pathBuffer,
    uint32_t  pathBufferSize);

// Like PrismaVR_GetViewBitmap but only copies if there's been a fresh paint
// since the last call. Returns true and fills the buffer if a new frame is
// available; returns false (without touching the buffer) if the view hasn't
// re-rendered since the last successful call. Lets a sister plugin avoid
// resubmitting identical pixels to OpenVR at every tick — important because
// SetOverlayRaw at 10 Hz × 8 MB enters a sticky error state after ~20 s, so
// we want to push pixels only when they actually changed.
//
// Size-query mode (outPixels=nullptr) behaves the same way as GetViewBitmap
// and DOES NOT consume the new-frame flag — call it any time to learn the
// current dimensions.
extern "C" PRISMAVR_API bool PrismaVR_GetViewBitmapIfNew(
    uint64_t  viewId,
    void*     outPixels,
    uint32_t  outBufSize,
    uint32_t* outWidth,
    uint32_t* outHeight,
    uint32_t* outStride);

// Inject a synthetic mouse event into the given view. Bypasses PrismaUI's
// input-capture / focus system entirely — intended for sister plugins that
// drive their own input (e.g. raycast from a VR controller onto an external
// overlay) and just need the underlying Ultralight View to receive clicks.
//
// Coordinates are in PIXELS, top-left origin, in the view's render-buffer space.
// Caller is responsible for translating from any other coordinate system
// (overlay UV, world space, screen pixels) into view pixels.
//
// Stable C ABI for the enum values:
//   eventType: 0 = move, 1 = button down, 2 = button up
//   button:    0 = none, 1 = left,        2 = middle, 3 = right
//
// Returns false if the view doesn't exist or arguments are out of range.
// The event is dispatched on PrismaUI's ultralight thread (the same path
// InputHandler uses internally), so calling this from any thread is safe.
extern "C" PRISMAVR_API bool PrismaVR_FireMouseEvent(
    uint64_t viewId,
    int      eventType,
    int      x,
    int      y,
    int      button);

// ---- Sister-plugin keyboard delivery ------------------------------------
//
// PrismaVR_DeliverChar / PrismaVR_DeliverVKey are gated on PrismaUI's OWN VR
// state (g_vrActive, g_vrOverlays non-empty, g_keyboardTargetViewId set,
// g_textInputFocused true) — none of which apply to a sister plugin that
// rendered the view onto its own controller-attached overlay. These
// *ToView variants take an explicit viewId and bypass the gates entirely:
// they look up the view, fire RawKeyDown + (Char|nothing) + KeyUp on the
// Ultralight thread, and return. The caller is responsible for knowing the
// view has an HTML input focused (typically by injecting its own JS focus
// tracker). Returns false if the view doesn't exist.
extern "C" PRISMAVR_API bool PrismaVR_DeliverCharToView(uint64_t viewId, wchar_t ch);
extern "C" PRISMAVR_API bool PrismaVR_DeliverVKeyToView(uint64_t viewId, int   vkCode);

// Inject a scroll-wheel event into the given view. deltaX/deltaY are in
// pixels (kType_ScrollByPixel). Positive Y typically scrolls content UP
// in Ultralight's convention. Bypasses PrismaUI's input gates, so sister
// plugins can drive scroll from a controller joystick or any other source.
extern "C" PRISMAVR_API bool PrismaVR_FireScrollToView(uint64_t viewId, int deltaX, int deltaY);
