#include "ViewManager.h"
#include "Core.h"
#include "InputHandler.h"
#include "Listeners.h"
#include "GPUDriver.h"

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
		std::string fileUrl = "file:///Data/PrismaUI/views/" + htmlPath;

		auto viewData = std::make_shared<Core::PrismaView>();
		viewData->id = newViewId;
		viewData->ultralightView = nullptr;
		viewData->htmlPathToLoad = fileUrl;
		viewData->isHidden = false;
		viewData->domReadyCallback = onDomReadyCallback;

		{
			std::unique_lock lock(viewsMutex);
			views[newViewId] = viewData;
		}

		logger::info("View [{}] creation requested for path: {}. Actual view will be created by UI thread.", newViewId, fileUrl);

		return newViewId;
	}

	void Show(const Core::PrismaViewId& viewId) {
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end()) {
			it->second->isHidden = false;
			logger::debug("View [{}] marked as Visible.", viewId);
		}
		else {
			logger::warn("Show: View ID [{}] not found.", viewId);
		}
	}

	void Hide(const Core::PrismaViewId& viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (viewData) {
			viewData->isHidden = true;
			ultralightThread.submit([view_ptr = viewData->ultralightView]() {
				if (view_ptr) view_ptr->Unfocus();
				});
			logger::debug("View [{}] marked as Hidden and Unfocused.", viewId);
		}
		else {
			logger::warn("Hide: View ID [{}] not found.", viewId);
		}
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

	bool Focus(const Core::PrismaViewId& viewId, bool pauseGame) {
		auto ui = RE::UI::GetSingleton();
		if (ui && ui->IsMenuOpen(RE::Console::MENU_NAME)) return false;

		std::shared_ptr<PrismaView> viewData = nullptr;
		{ std::shared_lock lock(viewsMutex); auto it = views.find(viewId); if (it != views.end()) viewData = it->second; }

		if (viewData && viewData->ultralightView) {
			if (viewData->isHidden.load()) {
				logger::warn("Focus: View [{}] is hidden, cannot focus.", viewId);
				return false;
			}

			std::vector<Core::PrismaViewId> viewsToUnfocus;
			{
				std::shared_lock lock(viewsMutex);
				for (const auto& pair : views) {
					if (pair.first != viewId) {
						auto future = ultralightThread.submit([view_ptr = pair.second->ultralightView]() -> bool {
							return view_ptr ? view_ptr->HasFocus() : false;
							});
						try {
							if (future.get()) {
								viewsToUnfocus.push_back(pair.first);
							}
						}
						catch (const std::exception& e) {
							logger::error("Exception checking focus state during Focus op: {}", e.what());
						}
					}
				}
			}

			for (const auto& idToUnfocus : viewsToUnfocus) {
				Unfocus(idToUnfocus);
			}

			ultralightThread.submit([view_ptr = viewData->ultralightView]() {
				if (view_ptr) view_ptr->Focus();
				});
			PrismaUI::InputHandler::EnableInputCapture(viewId);

			FocusMenu::Open();

			auto controlMap = RE::ControlMap::GetSingleton();
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kWheelZoom, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kJumping, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kMovement, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kActivate, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kPOVSwitch, false);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kVATS, false);

			if (pauseGame && ui) {
				ui->numPausesGame++;
				viewData->isPaused.store(true);
				logger::debug("Game paused for View [{}]", viewId);
			}

			logger::debug("Focus requested and input capture enabled for View [{}].", viewId);

			return true;
		}
		else if (viewData) {
			logger::warn("Focus: View [{}] Ultralight View is not ready.", viewId);
		}
		else {
			logger::warn("Focus: View ID [{}] not found.", viewId);
		}

		return false;
	}

	void Unfocus(const Core::PrismaViewId& viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{ std::shared_lock lock(viewsMutex); auto it = views.find(viewId); if (it != views.end()) viewData = it->second; }

		if (viewData && viewData->ultralightView) {
			if (viewData->isPaused.load()) {
				auto ui = RE::UI::GetSingleton();
				if (ui && ui->numPausesGame > 0) {
					ui->numPausesGame--;
					logger::debug("Game unpaused for View [{}]", viewId);
				}
				viewData->isPaused.store(false);
			}

			PrismaUI::InputHandler::DisableInputCapture(viewId);
			ultralightThread.submit([view_ptr = viewData->ultralightView]() {
				if (view_ptr) view_ptr->Unfocus();
				});

			FocusMenu::Close();

			auto controlMap = RE::ControlMap::GetSingleton();
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kWheelZoom, true);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, true);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kJumping, true);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kMovement, true);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kActivate, true);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kPOVSwitch, true);
			controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kVATS, true);

			logger::debug("Unfocus requested and input capture disabled for View [{}].", viewId);
		}
		else if (viewData) {
			if (viewData->isPaused.load()) {
				auto ui = RE::UI::GetSingleton();
				if (ui && ui->numPausesGame > 0) {
					ui->numPausesGame--;
					logger::debug("Game unpaused for View [{}]", viewId);
				}
				viewData->isPaused.store(false);
			}

			logger::warn("Unfocus: View [{}] Ultralight View is not ready.", viewId);
			PrismaUI::InputHandler::DisableInputCapture(viewId);
			FocusMenu::Close();
		}
		else {
			logger::warn("Unfocus: View ID [{}] not found.", viewId);
			PrismaUI::InputHandler::DisableInputCapture(0);
			FocusMenu::Close();
		}
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
			} else {
				logger::warn("Destroy: View ID [{}] not found after checking validity.", viewId);
				return;
			}
		}

		viewDataToDestroy->isHidden = true;

		{
			std::lock_guard<std::mutex> lock(jsCallbacksMutex);
			auto it = jsCallbacks.begin();
			while (it != jsCallbacks.end()) {
				if (it->first.first == viewId) {
					it = jsCallbacks.erase(it);
				} else {
					++it;
				}
			}
		}

		// Get RenderTarget before destroying the view on the UI thread
		ultralight::RenderTarget renderTarget;
		if (viewDataToDestroy->ultralightView) {
			renderTarget = viewDataToDestroy->ultralightView->render_target();
		}

		// Asynchronously destroy Ultralight resources on the UI thread
		ultralightThread.submit([viewData = viewDataToDestroy]() {
			if (viewData->ultralightView) {
				viewData->ultralightView->set_load_listener(nullptr);
				viewData->ultralightView->set_view_listener(nullptr);
				viewData->loadListener.reset();
				viewData->viewListener.reset();
				viewData->ultralightView = nullptr;
			}
		}).wait();

		// Synchronously destroy GPU resources on the main thread
		if (gpuDriver && renderTarget.texture_id != 0) {
			gpuDriver->DestroyRenderBuffer(renderTarget.render_buffer_id);
			gpuDriver->DestroyTexture(renderTarget.texture_id);
			logger::debug("Destroy: GPU resources for RenderTarget of View [{}] released.", viewId);
		}

		logger::info("Destroy: View [{}] successfully destroyed", viewId);
	}
}
