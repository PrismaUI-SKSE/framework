#include "Inspector.h"
#include "Core.h"
#include "ViewManager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstring>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace PrismaUI::Inspector {
	using namespace Core;

	namespace {
		std::once_flag gInspectorAssetCheckFlag;
		std::atomic<bool> gInspectorAssetsAvailable{ false };
	}

	// ========== Asset Management ==========

	void EnsureInspectorAssetsAvailability()
	{
		std::call_once(gInspectorAssetCheckFlag, []() {
			try {
				const auto inspectorPath = std::filesystem::current_path() / "inspector" / "Main.html";
				if (std::filesystem::exists(inspectorPath)) {
					gInspectorAssetsAvailable.store(true);
					logger::info("Ultralight inspector assets detected at {}", inspectorPath.string());
				}
				else {
					logger::warn("Ultralight inspector assets were not found at {}. Inspector view will not render unless the SDK inspector folder is copied next to the DLL.", inspectorPath.string());
				}
			}
			catch (const std::exception& e) {
				logger::warn("Failed to verify Ultralight inspector asset directory: {}", e.what());
			}
		});
	}

	bool AreInspectorAssetsAvailable()
	{
		EnsureInspectorAssetsAvailability();
		return gInspectorAssetsAvailable.load();
	}

	// ========== Resource Management ==========

	void ReleaseInspectorTexture(PrismaView* viewData)
	{
		if (!viewData) {
			return;
		}

		if (viewData->inspectorTextureView) {
			viewData->inspectorTextureView->Release();
			viewData->inspectorTextureView = nullptr;
		}

		if (viewData->inspectorTexture) {
			viewData->inspectorTexture->Release();
			viewData->inspectorTexture = nullptr;
		}

		viewData->inspectorTextureWidth = 0;
		viewData->inspectorTextureHeight = 0;
	}

	void DestroyInspectorResources(PrismaView* viewData)
	{
		if (!viewData) {
			return;
		}

		ReleaseInspectorTexture(viewData);

		{
			std::lock_guard bufferLock(viewData->inspectorBufferMutex);
			viewData->inspectorPixelBuffer.clear();
			viewData->inspectorPixelBuffer.shrink_to_fit();
			viewData->inspectorBufferWidth = 0;
			viewData->inspectorBufferHeight = 0;
			viewData->inspectorBufferStride = 0;
		}

		viewData->inspectorFrameReady.store(false);
		viewData->inspectorPointerHover.store(false);
	}

	// ========== Inspector Lifecycle ==========

	void CreateInspectorView(const PrismaViewId& viewId)
	{
		EnsureInspectorAssetsAvailability();
		if (!gInspectorAssetsAvailable.load()) {
			logger::warn("View [{}]: Inspector assets were not found. Copy the Ultralight inspector folder next to PrismaUI.dll to enable the inspector.", viewId);
			return;
		}

		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			logger::warn("CreateInspectorView: View ID [{}] not found.", viewId);
			return;
		}

		if (viewData->inspectorView) {
			logger::info("View [{}]: Inspector view already exists.", viewId);
			return;
		}

		if (!viewData->ultralightView) {
			logger::warn("View [{}]: Cannot create inspector because Ultralight view is not ready yet.", viewId);
			return;
		}

		try {
			auto createInspector = [view = viewData]() {
				if (view->ultralightView) {
					view->ultralightView->CreateLocalInspectorView();
				}
			};

			if (ultralightThread.IsWorkerThread()) {
				createInspector();
			}
			else {
				auto future = ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::MEDIUM, createInspector);
				future.get();
			}
			logger::info("View [{}]: Inspector creation requested.", viewId);
		}
		catch (const std::exception& e) {
			logger::error("View [{}]: Exception while creating inspector view: {}", viewId, e.what());
		}
	}

	void SetInspectorVisibility(const PrismaViewId& viewId, bool visible)
	{
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			logger::warn("SetInspectorVisibility: View ID [{}] not found.", viewId);
			return;
		}

		if (!viewData->inspectorView && visible) {
			CreateInspectorView(viewId);
		}

		if (!viewData->inspectorView) {
			logger::warn("View [{}]: Inspector view is not available to {}.", viewId, visible ? "show" : "hide");
			return;
		}

		viewData->inspectorVisible.store(visible);
		viewData->inspectorPointerHover.store(false);

		if (visible && viewData->ultralightView && ViewManager::HasFocus(viewId)) {
			auto future = ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::MEDIUM, [view = viewData]() {
				if (view->inspectorView) {
					view->inspectorView->Focus();
				}
				if (view->ultralightView) {
					view->ultralightView->Unfocus();
				}
			});
			future.wait();
		}

		logger::info("View [{}]: Inspector visibility set to {}.", viewId, visible);
	}

	bool IsInspectorVisible(const PrismaViewId& viewId)
	{
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end() && it->second) {
			return it->second->inspectorVisible.load();
		}
		return false;
	}

	void SetInspectorBounds(const PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width, uint32_t height)
	{
		width = std::max<uint32_t>(width, 32u);
		height = std::max<uint32_t>(height, 32u);

		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			logger::warn("SetInspectorBounds: View ID [{}] not found.", viewId);
			return;
		}

		if (!viewData->inspectorView) {
			logger::warn("View [{}]: Cannot set inspector bounds because inspector is not available.", viewId);
			return;
		}

		const float screenW = static_cast<float>(screenSize.width ? screenSize.width : width);
		const float screenH = static_cast<float>(screenSize.height ? screenSize.height : height);
		const float maxX = std::max(0.0f, screenW - static_cast<float>(width));
		const float maxY = std::max(0.0f, screenH - static_cast<float>(height));

		viewData->inspectorPosX = std::clamp(topLeftX, 0.0f, maxX);
		viewData->inspectorPosY = std::clamp(topLeftY, 0.0f, maxY);
		viewData->inspectorDisplayWidth = width;
		viewData->inspectorDisplayHeight = height;
		viewData->inspectorPointerHover.store(false);

		try {
			auto resizeInspector = [view = viewData, width, height]() {
				if (view->inspectorView) {
					view->inspectorView->Resize(width, height);
				}
			};

			if (ultralightThread.IsWorkerThread()) {
				resizeInspector();
			}
			else {
				auto future = ultralightThread.submit_with_priority(SingleThreadExecutor::Priority::MEDIUM, resizeInspector);
				future.wait();
			}
			logger::info("View [{}]: Inspector bounds set to ({}, {}) with size {}x{}", viewId, topLeftX, topLeftY, width, height);
		}
		catch (const std::exception& e) {
			logger::error("View [{}]: Exception while setting inspector bounds: {}", viewId, e.what());
		}
	}

	// ========== Rendering Pipeline ==========

	void RenderInspectorView(std::shared_ptr<PrismaView> viewData)
	{
		if (!viewData || !viewData->inspectorView || !viewData->inspectorVisible.load() || viewData->isHidden.load()) {
			return;
		}

		Surface* surface = viewData->inspectorView->surface();
		if (surface) {
			CopyInspectorBitmapToBuffer(viewData);
		}
	}

	void CopyInspectorBitmapToBuffer(std::shared_ptr<PrismaView> viewData)
	{
		if (!viewData || !viewData->inspectorView) {
			return;
		}

		Surface* surface = viewData->inspectorView->surface();
		if (!surface) {
			return;
		}

		// For inspector views, try BitmapSurface
		BitmapSurface* bitmapSurface = static_cast<BitmapSurface*>(surface);
		if (!bitmapSurface) {
			return;
		}

		RefPtr<Bitmap> bitmap = bitmapSurface->bitmap();
		if (!bitmap || bitmap->IsEmpty()) {
			return;
		}

		const void* pixels = bitmap->LockPixels();
		if (!pixels) {
			bitmap->UnlockPixels();
			return;
		}

		const uint32_t width = bitmap->width();
		const uint32_t height = bitmap->height();
		const uint32_t stride = bitmap->row_bytes();
		const size_t dataSize = stride * height;

		{
			std::lock_guard<std::mutex> lock(viewData->inspectorBufferMutex);

			if (viewData->inspectorPixelBuffer.size() != dataSize) {
				viewData->inspectorPixelBuffer.resize(dataSize);
			}

			std::memcpy(viewData->inspectorPixelBuffer.data(), pixels, dataSize);

			viewData->inspectorBufferWidth = width;
			viewData->inspectorBufferHeight = height;
			viewData->inspectorBufferStride = stride;
			viewData->inspectorFrameReady.store(true);
		}

		bitmap->UnlockPixels();
	}

	void CopyInspectorPixelsToTexture(PrismaView* viewData, void* pixels, uint32_t width, uint32_t height, uint32_t stride)
	{
		if (!viewData || !pixels || !d3dContext || !d3dDevice) {
			return;
		}

		if (width == 0 || height == 0) {
			return;
		}

		// Recreate texture if size changed
		if (!viewData->inspectorTexture ||
			viewData->inspectorTextureWidth != width ||
			viewData->inspectorTextureHeight != height) {

			if (viewData->inspectorTextureView) {
				viewData->inspectorTextureView->Release();
				viewData->inspectorTextureView = nullptr;
			}
			if (viewData->inspectorTexture) {
				viewData->inspectorTexture->Release();
				viewData->inspectorTexture = nullptr;
			}

			D3D11_TEXTURE2D_DESC texDesc = {};
			texDesc.Width = width;
			texDesc.Height = height;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DYNAMIC;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			HRESULT hr = d3dDevice->CreateTexture2D(&texDesc, nullptr, &viewData->inspectorTexture);
			if (FAILED(hr)) {
				logger::error("Failed to create inspector D3D11 texture for View [{}]: HRESULT={:X}", viewData->id, static_cast<unsigned>(hr));
				return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = texDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;

			hr = d3dDevice->CreateShaderResourceView(viewData->inspectorTexture, &srvDesc, &viewData->inspectorTextureView);
			if (FAILED(hr)) {
				logger::error("Failed to create inspector shader resource view for View [{}]: HRESULT={:X}", viewData->id, static_cast<unsigned>(hr));
				viewData->inspectorTexture->Release();
				viewData->inspectorTexture = nullptr;
				return;
			}

			viewData->inspectorTextureWidth = width;
			viewData->inspectorTextureHeight = height;
		}

		// Upload pixels to texture
		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = d3dContext->Map(viewData->inspectorTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (SUCCEEDED(hr)) {
			const uint8_t* src = static_cast<const uint8_t*>(pixels);
			uint8_t* dst = static_cast<uint8_t*>(mapped.pData);

			for (uint32_t row = 0; row < height; ++row) {
				std::memcpy(dst + row * mapped.RowPitch, src + row * stride, width * 4);
			}

			d3dContext->Unmap(viewData->inspectorTexture, 0);
		}
		else {
			logger::error("Failed to map inspector texture for View [{}]: HRESULT={:X}", viewData->id, static_cast<unsigned>(hr));
		}
	}
}
