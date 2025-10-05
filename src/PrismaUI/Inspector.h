#pragma once

#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#include <memory>

namespace PrismaUI::Core {
	typedef uint64_t PrismaViewId;
	struct PrismaView;
}

namespace PrismaUI::Inspector {
	using namespace ultralight;

	// Inspector lifecycle management
	void CreateInspectorView(const Core::PrismaViewId& viewId);
	void SetInspectorVisibility(const Core::PrismaViewId& viewId, bool visible);
	bool IsInspectorVisible(const Core::PrismaViewId& viewId);
	void SetInspectorBounds(const Core::PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width, uint32_t height);

	// Inspector rendering (called from ViewRenderer)
	void RenderInspectorView(std::shared_ptr<Core::PrismaView> viewData);
	void CopyInspectorBitmapToBuffer(std::shared_ptr<Core::PrismaView> viewData);
	void CopyInspectorPixelsToTexture(Core::PrismaView* viewData, void* pixels, uint32_t width, uint32_t height, uint32_t stride);

	// Inspector resource management
	void ReleaseInspectorTexture(Core::PrismaView* viewData);
	void DestroyInspectorResources(Core::PrismaView* viewData);

	// Inspector utilities
	void EnsureInspectorAssetsAvailability();
	bool AreInspectorAssetsAvailable();
}
