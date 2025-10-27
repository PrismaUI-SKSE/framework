#include "ViewManager.h"
#include "Core.h"
#include "InputHandler.h"
#include "Listeners.h"
#include "ViewOperationQueue.h"

namespace PrismaUI::ViewManager {
	using namespace Core;

	Core::PrismaViewId Create(const std::string& htmlPath, std::function<void(Core::PrismaViewId)> onDomReadyCallback) {
		bool expected_init = false;
		if (coreInitialized.compare_exchange_strong(expected_init, true)) {
			Core::InitializeCoreSystem();
			if (!renderer) {
				coreInitialized = false;
				logger::critical("Core initialization failed: Renderer not created.");
				throw std::runtime_error("PrismaUI Core Renderer initialization failed.");
			}
		}
		else if (!renderer) {
			logger::critical("Cannot create HTML view: Core Renderer is null despite initialization flag.");
			throw std::runtime_error("PrismaUI Core Renderer is unexpectedly null.");
		}

		Core::PrismaViewId newViewId = generator.generate();
		
		// Check if htmlPath is a website URL (starts with http:// or https://)
		std::string fileUrl;
		if (htmlPath.substr(0, 7) == "http://" || htmlPath.substr(0, 8) == "https://") {
			fileUrl = htmlPath;
		} else {
			fileUrl = "file:///Data/PrismaUI/views/" + htmlPath;
		}

		auto viewData = std::make_shared<Core::PrismaView>();
		viewData->id = newViewId;
		viewData->ultralightView = nullptr;
		viewData->htmlPathToLoad = fileUrl;
		viewData->isHidden = false;
		viewData->domReadyCallback = onDomReadyCallback;

		{
			std::unique_lock lock(viewsMutex);
			int maxOrder = -1;
			if (views.empty()) {
				viewData->order = 0;
			} else {
				for (const auto& pair : views) {
					if (pair.second->order > maxOrder) {
						maxOrder = pair.second->order;
					}
				}
				viewData->order = maxOrder + 1;
			}
			views[newViewId] = viewData;
		}

		logger::info("View [{}] creation requested for path: {} with order <{}>. Actual view will be created by UI thread.", newViewId, fileUrl, viewData->order);

		return newViewId;
	}

	// Helper function to perform unfocus operations (used by Hide, Unfocus, and Focus)
	// closeFocusMenu: whether to close FocusMenu (false when switching focus between views)
	static void PerformUnfocusOperations(const Core::PrismaViewId& viewId, std::shared_ptr<PrismaView> viewData, bool closeFocusMenu = true) {
		if (!viewData || !viewData->ultralightView) {
			return;
		}

		if (viewData->isPaused.load()) {
			auto ui = RE::UI::GetSingleton();
			if (ui && ui->numPausesGame > 0) {
				ui->numPausesGame--;
			}
			viewData->isPaused.store(false);
		}

		PrismaUI::InputHandler::DisableInputCapture(viewId);
		viewData->ultralightView->Unfocus();
		
		if (closeFocusMenu) {
			FocusMenu::Close();
		}

		auto controlMap = RE::ControlMap::GetSingleton();
		controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kWheelZoom, true);
		controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, true);
		controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kJumping, true);
		controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kMovement, true);
		controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kActivate, true);
		controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kPOVSwitch, true);
		controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kVATS, true);
	}

	void Show(const Core::PrismaViewId& viewId) {
		if (!ViewManager::IsValid(viewId)) {
			logger::warn("Show: View ID [{}] not found.", viewId);
			return;
		}

		ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
			std::shared_ptr<PrismaView> viewData = nullptr;
			{
				std::shared_lock lock(viewsMutex);
				auto it = views.find(viewId);
				if (it != views.end()) {
					viewData = it->second;
				}
			}

			if (viewData) {
				if (!viewData->isHidden.load()) {
					logger::debug("Show: View [{}] is already visible.", viewId);
					return;
				}
				viewData->isHidden = false;
				logger::debug("View [{}] marked as Visible.", viewId);
			}
		});
	}

	void Hide(const Core::PrismaViewId& viewId) {
		if (!ViewManager::IsValid(viewId)) {
			logger::warn("Hide: View ID [{}] not found.", viewId);
			return;
		}

		ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
			std::shared_ptr<PrismaView> viewData = nullptr;
			{
				std::shared_lock lock(viewsMutex);
				auto it = views.find(viewId);
				if (it != views.end()) {
					viewData = it->second;
				}
			}

			if (viewData) {
				if (viewData->isHidden.load()) {
					logger::debug("Hide: View [{}] is already hidden.", viewId);
					return;
				}

				// If view has focus, unfocus it first
				if (viewData->ultralightView && viewData->ultralightView->HasFocus()) {
					PerformUnfocusOperations(viewId, viewData);
					logger::debug("Hide: View [{}] was focused, unfocused it.", viewId);
				}

				viewData->isHidden = true;
				logger::debug("View [{}] marked as Hidden.", viewId);
			}
		});
	}

	bool IsHidden(const Core::PrismaViewId& viewId) {
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end()) {
			return it->second->isHidden.load();
		}
		logger::warn("IsHidden: View ID [{}] not found.", viewId);
		return true;
	}

	bool IsValid(const Core::PrismaViewId& viewId) {
		std::shared_lock lock(viewsMutex);
		return views.find(viewId) != views.end();
	}

	bool Focus(const Core::PrismaViewId& viewId, bool pauseGame, bool disableFocusMenu) {
		if (!ViewManager::IsValid(viewId)) {
			logger::warn("Focus: View ID [{}] not found.", viewId);
			return false;
		}

		ViewOperationQueue::EnqueueOperation(viewId, [viewId, pauseGame, disableFocusMenu]() {
			std::shared_ptr<PrismaView> viewData = nullptr;
			{
				std::shared_lock lock(viewsMutex);
				auto it = views.find(viewId);
				if (it != views.end()) {
					viewData = it->second;
				}
			}

			if (!viewData || !viewData->ultralightView) {
				logger::warn("Focus: View [{}] or its Ultralight View is not ready.", viewId);
				return;
			}

			if (viewData->isHidden.load()) {
				logger::warn("Focus: View [{}] is hidden, cannot focus.", viewId);
				return;
			}

			if (viewData->ultralightView->HasFocus()) {
				logger::debug("Focus: View [{}] already has focus.", viewId);
				return;
			}

			// Unfocus other views
			std::vector<Core::PrismaViewId> viewsToUnfocus;
			{
				std::shared_lock lock(viewsMutex);
				for (const auto& pair : views) {
					if (pair.first != viewId && pair.second->ultralightView && pair.second->ultralightView->HasFocus()) {
						viewsToUnfocus.push_back(pair.first);
					}
				}
			}

			for (const auto& idToUnfocus : viewsToUnfocus) {
				ViewOperationQueue::EnqueueOperation(idToUnfocus, [idToUnfocus]() {
					std::shared_ptr<PrismaView> vd = nullptr;
					{
						std::shared_lock lock(viewsMutex);
						auto it = views.find(idToUnfocus);
						if (it != views.end()) {
							vd = it->second;
						}
					}

					if (vd && vd->ultralightView) {
						// Don't close FocusMenu when switching focus between views
						PerformUnfocusOperations(idToUnfocus, vd, false);
						logger::debug("Unfocus: View [{}] unfocused (focus switching).", idToUnfocus);
					}
				});
			}

			// Focus this view
			viewData->ultralightView->Focus();
			PrismaUI::InputHandler::EnableInputCapture(viewId);
			
			if (!disableFocusMenu) {
				FocusMenu::Open();
			}

			auto controlMap = RE::ControlMap::GetSingleton();
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kWheelZoom, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kJumping, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kMovement, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kActivate, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kPOVSwitch, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kVATS, false);

			if (pauseGame) {
				auto ui = RE::UI::GetSingleton();
				if (ui) {
					ui->numPausesGame++;
					viewData->isPaused.store(true);
					logger::debug("Game paused for View [{}]", viewId);
				}
			}

			logger::debug("Focus: View [{}] focused successfully.", viewId);
		});

		return true;
	}

	void Unfocus(const Core::PrismaViewId& viewId) {
		if (!ViewManager::IsValid(viewId)) {
			logger::warn("Unfocus: View ID [{}] not found.", viewId);
			return;
		}

		ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
			std::shared_ptr<PrismaView> viewData = nullptr;
			{
				std::shared_lock lock(viewsMutex);
				auto it = views.find(viewId);
				if (it != views.end()) {
					viewData = it->second;
				}
			}

			if (!viewData) {
				logger::warn("Unfocus: View [{}] not found during operation execution.", viewId);
				PrismaUI::InputHandler::DisableInputCapture(0);
				FocusMenu::Close();
				return;
			}

			if (!viewData->ultralightView) {
				logger::warn("Unfocus: View [{}] Ultralight View is not ready.", viewId);
				if (viewData->isPaused.load()) {
					auto ui = RE::UI::GetSingleton();
					if (ui && ui->numPausesGame > 0) {
						ui->numPausesGame--;
					}
					viewData->isPaused.store(false);
				}
				PrismaUI::InputHandler::DisableInputCapture(viewId);
				FocusMenu::Close();
				return;
			}

			if (!viewData->ultralightView->HasFocus()) {
				logger::debug("Unfocus: View [{}] does not have focus.", viewId);
				return;
			}

			PerformUnfocusOperations(viewId, viewData);
			logger::debug("Unfocus: View [{}] unfocused successfully.", viewId);
		});
	}

	bool HasFocus(const Core::PrismaViewId& viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (viewData) {
			auto future = ultralightThread.submit([view_ptr = viewData->ultralightView]() -> bool {
				return view_ptr ? view_ptr->HasFocus() : false;
				});
			try {
				return future.get();
			}
			catch (const std::exception& e) {
				logger::error("Exception getting focus state for View [{}]: {}", viewId, e.what());
				return false;
			}
		}
		else {
			logger::warn("HasFocus: View ID [{}] not found.", viewId);
			return false;
		}
	}

	bool ViewHasInputFocus(const Core::PrismaViewId& viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (viewData && viewData->ultralightView) {
			auto future = ultralightThread.submit([view_ptr = viewData->ultralightView]() -> bool {
				if (view_ptr) {
					return view_ptr->HasInputFocus();
				}
				return false;
				});
			try {
				return future.get();
			}
			catch (const std::exception& e) {
				logger::error("View [{}]: Exception in ViewHasInputFocus: {}", viewId, e.what());
				return false;
			}
		}
		return false;
	}

	void SetScrollingPixelSize(const Core::PrismaViewId& viewId, int pixelSize) {
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end()) {
			if (pixelSize <= 0) {
				logger::warn("SetScrollingPixelSize: Invalid pixel size {} for view [{}]. Must be > 0. Using default.", pixelSize, viewId);
				it->second->scrollingPixelSize = 16;
			}
			else {
				it->second->scrollingPixelSize = pixelSize;
				logger::debug("SetScrollingPixelSize: Set {} pixels per scroll line for view [{}]", pixelSize, viewId);
			}
		}
		else {
			logger::warn("SetScrollingPixelSize: View ID [{}] not found.", viewId);
		}
	}

	int GetScrollingPixelSize(const Core::PrismaViewId& viewId) {
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end()) {
			return it->second->scrollingPixelSize;
		}
		logger::warn("GetScrollingPixelSize: View ID [{}] not found, returning default.", viewId);
		return 28;
	}

	void Destroy(const Core::PrismaViewId& viewId) {
		logger::info("Destroy: Beginning destruction of View [{}]", viewId);

		if (!ViewManager::IsValid(viewId)) {
			logger::warn("Destroy: View ID [{}] not found.", viewId);
			return;
		}

		// Clear pending operations for this view to prevent execution after destruction
		ViewOperationQueue::ClearOperations(viewId);
		logger::debug("Destroy: Cleared pending operations for View [{}]", viewId);

		if (HasFocus(viewId)) {
			logger::debug("Destroy: View [{}] has focus, unfocusing first.", viewId);
			Unfocus(viewId);
		}

		std::shared_ptr<PrismaView> viewDataToDestroy = nullptr;
		{
			std::unique_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewDataToDestroy = std::move(it->second);
				views.erase(it);
				logger::debug("Destroy: Removed View [{}] from views map", viewId);
			}
			else {
				logger::warn("Destroy: View ID [{}] not found after checking validity.", viewId);
				return;
			}
		}

		viewDataToDestroy->isHidden = true;
		logger::debug("Destroy: Marked View [{}] as hidden", viewId);

		{
			std::lock_guard<std::mutex> lock(jsCallbacksMutex);
			logger::debug("Destroy: Removing JavaScript callbacks for View [{}]", viewId);

			auto it = jsCallbacks.begin();
			size_t removedCallbacks = 0;

			while (it != jsCallbacks.end()) {
				if (it->first.first == viewId) {
					it = jsCallbacks.erase(it);
					removedCallbacks++;
				}
				else {
					++it;
				}
			}

			if (removedCallbacks > 0) {
				logger::debug("Destroy: Removed {} JavaScript callback(s) for View [{}]",
					removedCallbacks, viewId);
			}
		}

		logger::debug("Destroy: Cleaning up Ultralight resources (on UI thread) for View [{}]", viewId);
		auto ultralightCleanupFuture = ultralightThread.submit([viewId, viewData = viewDataToDestroy]() {
			try {
				logger::debug("Destroy: Beginning Ultralight resources cleanup for View [{}]", viewId);

				if (viewData->ultralightView) {
					logger::debug("Destroy: Detaching listeners for View [{}]", viewId);
					viewData->ultralightView->set_load_listener(nullptr);
					viewData->ultralightView->set_view_listener(nullptr);

					viewData->loadListener.reset();
					viewData->viewListener.reset();

					viewData->ultralightView = nullptr;
					logger::debug("Destroy: Ultralight View object released for View [{}]", viewId);
				}

				{
					std::lock_guard lock(viewData->bufferMutex);
					viewData->pixelBuffer.clear();
					viewData->pixelBuffer.shrink_to_fit();
					viewData->bufferWidth = 0;
					viewData->bufferHeight = 0;
					viewData->bufferStride = 0;
					logger::debug("Destroy: Pixel buffer cleared for View [{}]", viewId);
				}

				viewData->isLoadingFinished = false;
				viewData->newFrameReady = false;

				logger::debug("Destroy: Ultralight resources for View [{}] cleaned up successfully", viewId);

				return true;
			}
			catch (const std::exception& e) {
				logger::error("Destroy: Exception during Ultralight resource cleanup for View [{}]: {}", viewId, e.what());
				return false;
			}
			catch (...) {
				logger::error("Destroy: Unknown exception during Ultralight resource cleanup for View [{}]", viewId);
				return false;
			}
			});

		try {
			bool ultralight_success = ultralightCleanupFuture.get();
			if (ultralight_success) {
				logger::debug("Destroy: Ultralight resources cleanup completed successfully for View [{}]", viewId);
			}
			else {
				logger::warn("Destroy: Ultralight resources cleanup reported failure for View [{}]", viewId);
			}
		}
		catch (const std::exception& e) {
			logger::error("Destroy: Exception waiting for Ultralight cleanup for View [{}]: {}", viewId, e.what());
		}

		bool hasD3DResources = (viewDataToDestroy->texture != nullptr || viewDataToDestroy->textureView != nullptr);

		if (hasD3DResources) {
			logger::debug("Destroy: D3D resources present for View [{}], forcing manual cleanup", viewId);

			if (viewDataToDestroy->textureView) {
				logger::debug("Destroy: Releasing textureView for View [{}]", viewId);
				viewDataToDestroy->textureView->Release();
				viewDataToDestroy->textureView = nullptr;
			}

			if (viewDataToDestroy->texture) {
				logger::debug("Destroy: Releasing texture for View [{}]", viewId);
				viewDataToDestroy->texture->Release();
				viewDataToDestroy->texture = nullptr;
			}

			viewDataToDestroy->textureWidth = 0;
			viewDataToDestroy->textureHeight = 0;

			logger::debug("Destroy: D3D resources released for View [{}]", viewId);
		}
		else {
			logger::debug("Destroy: No D3D resources to release for View [{}]", viewId);
		}

		viewDataToDestroy->pendingResourceRelease = false;

		logger::info("Destroy: View [{}] successfully destroyed", viewId);
	}

	void SetOrder(const Core::PrismaViewId& viewId, int order) {
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end()) {
			it->second->order = order;
			logger::debug("SetOrder: Set order {} for view [{}]", order, viewId);
		}
		else {
			logger::warn("SetOrder: View ID [{}] not found.", viewId);
		}
	}

	int GetOrder(const Core::PrismaViewId& viewId) {
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end()) {
			return it->second->order;
		}
		logger::warn("GetOrder: View ID [{}] not found, returning -1.", viewId);
		return -1;
	}

	bool HasAnyActiveFocus() {
		std::shared_lock lock(viewsMutex);
		
		for (const auto& pair : views) {
			if (pair.second && pair.second->ultralightView) {
				auto future = ultralightThread.submit([view_ptr = pair.second->ultralightView]() -> bool {
					return view_ptr ? view_ptr->HasFocus() : false;
				});
				
				try {
					if (future.get()) {
						return true;
					}
				} catch (const std::exception& e) {
					logger::error("HasAnyActiveFocus: Exception checking focus for view: {}", e.what());
				}
			}
		}
		
		return false;
	}
}
