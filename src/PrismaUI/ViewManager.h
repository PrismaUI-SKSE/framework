#pragma once

#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#include <Ultralight/StringSTL.h>
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>

namespace PrismaUI::Core {
	typedef uint64_t PrismaViewId;
}

namespace PrismaUI::ViewManager {
	using namespace ultralight;

	Core::PrismaViewId Create(const std::string& htmlPath, std::function<void(Core::PrismaViewId)> onDomReadyCallback = nullptr);
	void Show(const Core::PrismaViewId& viewId);
	void Hide(const Core::PrismaViewId& viewId);
	bool IsHidden(const Core::PrismaViewId& viewId);
	bool Focus(const Core::PrismaViewId& viewId, bool pauseGame = false);
	void Unfocus(const Core::PrismaViewId& viewId);
	bool HasFocus(const Core::PrismaViewId& viewId);
	bool ViewHasInputFocus(const Core::PrismaViewId& viewId);
	void Destroy(const Core::PrismaViewId& viewId);
	bool IsValid(const Core::PrismaViewId& viewId);
	void SetScrollingPixelSize(const Core::PrismaViewId& viewId, int pixelSize);
	int GetScrollingPixelSize(const Core::PrismaViewId& viewId);
	void SetOrder(const Core::PrismaViewId& viewId, int order);
	int GetOrder(const Core::PrismaViewId& viewId);

	// Inspector View functions
	void CreateInspectorView(const Core::PrismaViewId& viewId);
	void SetInspectorVisibility(const Core::PrismaViewId& viewId, bool visible);
	bool IsInspectorVisible(const Core::PrismaViewId& viewId);
	void SetInspectorBounds(const Core::PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width, uint32_t height);
}
