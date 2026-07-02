#pragma once
// PrismaVR_Bridge.h — Thin accessor layer for Prisma UI internals.
// This is the ONLY VR file that directly touches PrismaView fields.
// If Prisma renames a field, only this file needs a one-line fix.

#include "Core.h"

#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

#include <memory>
#include <vector>

namespace PrismaVR_Bridge {

	struct ViewInfo {
		std::shared_ptr<PrismaUI::Core::PrismaView> view;
		uint64_t id;
	};

	// --- Accessors (one field each, trivial to update if Prisma changes) ---

	inline ID3D11Texture2D* GetTexture(PrismaUI::Core::PrismaView* v) {
		return v ? v->texture : nullptr;
	}

	inline uint32_t GetTextureWidth(PrismaUI::Core::PrismaView* v) {
		return v ? v->textureWidth : 0;
	}

	inline uint32_t GetTextureHeight(PrismaUI::Core::PrismaView* v) {
		return v ? v->textureHeight : 0;
	}

	inline bool IsViewHidden(PrismaUI::Core::PrismaView* v) {
		return v ? v->isHidden.load() : true;
	}

	inline ID3D11Device* GetD3DDevice() {
		return PrismaUI::Core::d3dDevice;
	}

	// --- View enumeration (returns shared_ptrs for thread safety) ---

	inline std::vector<ViewInfo> GetVisibleViews() {
		std::vector<ViewInfo> result;
		std::shared_lock lock(PrismaUI::Core::viewsMutex);
		for (auto& [id, viewPtr] : PrismaUI::Core::views) {
			if (viewPtr && !viewPtr->isHidden.load()) {
				result.push_back({ viewPtr, id });
			}
		}
		return result;
	}

	// --- Event injection (submits to Ultralight thread, fire-and-forget) ---

	inline void FireMouseEvent(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	                           const ultralight::MouseEvent& ev) {
		if (!view || !view->ultralightView) return;
		PrismaUI::Core::ultralightThread.submit([view, ev]() {
			if (view->ultralightView)
				view->ultralightView->FireMouseEvent(ev);
		});
	}

	inline void FireScrollEvent(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	                            const ultralight::ScrollEvent& ev) {
		if (!view || !view->ultralightView) return;
		PrismaUI::Core::ultralightThread.submit([view, ev]() {
			if (view->ultralightView)
				view->ultralightView->FireScrollEvent(ev);
		});
	}

	inline void FireKeyEvent(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	                         const ultralight::KeyEvent& ev) {
		if (!view || !view->ultralightView) return;
		PrismaUI::Core::ultralightThread.submit([view, ev]() {
			if (view->ultralightView)
				view->ultralightView->FireKeyEvent(ev);
		});
	}

	// --- Ultralight view focus (required for <input> fields to accept keyboard) ---

	inline void FocusView(std::shared_ptr<PrismaUI::Core::PrismaView> view) {
		if (!view || !view->ultralightView) return;
		PrismaUI::Core::ultralightThread.submit([view]() {
			if (view->ultralightView)
				view->ultralightView->Focus();
		});
	}

	inline void UnfocusView(std::shared_ptr<PrismaUI::Core::PrismaView> view) {
		if (!view || !view->ultralightView) return;
		PrismaUI::Core::ultralightThread.submit([view]() {
			if (view->ultralightView)
				view->ultralightView->Unfocus();
		});
	}

	// --- Game control masking (split: combat vs movement) ---
	// Works on both OpenComposite and SteamVR via Skyrim's ControlMap.
	//
	// ToggleControls mutates the game's live input state and is NOT safe from the
	// render thread (PrismaVR::OnFrame) — it races the game thread's input
	// processing. All toggles are queued through the SKSE task interface so they
	// execute on the game thread; AddTask preserves FIFO order, so rapid
	// mask/unmask sequences from laser clicks apply in the order they were issued.

	inline void ToggleControlsOnGameThread(RE::UserEvents::USER_EVENT_FLAG flag, bool enable) {
		SKSE::GetTaskInterface()->AddTask([flag, enable]() {
			auto controlMap = RE::ControlMap::GetSingleton();
			if (!controlMap) return;
			controlMap->ToggleControls(flag, enable, false);
		});
	}

	inline void MaskCombatControls() {
		ToggleControlsOnGameThread(RE::UserEvents::USER_EVENT_FLAG::kFighting, false);
	}

	inline void UnmaskCombatControls() {
		ToggleControlsOnGameThread(RE::UserEvents::USER_EVENT_FLAG::kFighting, true);
	}

	inline void MaskMovement() {
		ToggleControlsOnGameThread(RE::UserEvents::USER_EVENT_FLAG::kMovement, false);
	}

	inline void UnmaskMovement() {
		ToggleControlsOnGameThread(RE::UserEvents::USER_EVENT_FLAG::kMovement, true);
	}

	// --- JavaScript execution (for injecting VR cursor into HTML views) ---

	inline void RunJavaScript(std::shared_ptr<PrismaUI::Core::PrismaView> view,
	                          const std::string& script) {
		if (!view || !view->ultralightView) return;
		PrismaUI::Core::ultralightThread.submit([view, script]() {
			if (view->ultralightView)
				view->ultralightView->EvaluateScript(
					ultralight::String(script.c_str()), nullptr, ultralight::String(""));
		});
	}

} // namespace PrismaVR_Bridge
