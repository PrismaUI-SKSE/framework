#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct tagRECT;
typedef struct tagRECT RECT;

// 3D item model preview: renders an item NIF to an offscreen RT, shown as a
// screen-space sprite (flat) or a second OpenVR overlay (VR) over a panel rect.
namespace PrismaUI::ModelPreview {

	// Snapshot of the host panel's VR overlay state, filled by PrismaVR each frame.
	struct PanelInfo {
		bool valid = false;
		float pos[3] = {};
		float fwd[3] = {};
		float up[3] = {};
		float widthMeters = 0.0f;
		uint32_t texWidth = 0;
		uint32_t texHeight = 0;
	};

	// One ready preview's texture and where to draw it (flat composite).
	struct FlatDraw {
		ID3D11ShaderResourceView* srv = nullptr;
		RECT dest;
	};

	// PrismaVR provides this so TickVR can fetch each preview's panel transform.
	using PanelInfoFn = bool (*)(uint64_t viewId, PanelInfo& out);

	// Ini flag (Data/SKSE/Plugins/PrismaUI_ModelPreview.ini), cached on first call.
	bool Enabled();

	// JS thread. Arg: {"plugin","localId"/"formId","x","y","w","h", optional "id" + view params}.
	// "id" lets one view own several previews at once (default "" = one preview).
	void Show(uint64_t viewId, const std::string& jsonArgs);
	void Hide(uint64_t viewId, const std::string& jsonArgs);  // arg {"id":...}; no id clears the view

	// Render thread. Any live preview's view (0 = none); kept for compatibility.
	uint64_t ActiveViewId();

	// Render thread, once per frame before DrawViews: consume requests, adopt loads, render.
	void TickCore(ID3D11Device* dev, ID3D11DeviceContext* ctx);

	// Render thread, from flat composite: every ready preview for this view.
	void GetFlatOverlays(uint64_t viewId, std::vector<FlatDraw>& out);

	// Render thread, from PrismaVR::OnFrame after panel transforms are final.
	void TickVR(void* overlayIface, PanelInfoFn lookup);

	// Any thread. Clears all previews owned by this view.
	void OnPanelDestroyed(uint64_t viewId);

	// Plugin unload: join worker, destroy overlay, release D3D resources. Idempotent.
	void Shutdown();
}
