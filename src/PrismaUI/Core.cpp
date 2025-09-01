#include "Core.h"
#include "ViewRenderer.h"
#include "ViewManager.h"
#include "Listeners.h"
#include "InputHandler.h"
#include "Communication.h"

namespace PrismaUI::Core {
	using namespace PrismaUI::Listeners;
	using namespace PrismaUI::ViewRenderer;
	using namespace PrismaUI::ViewManager;
	using namespace PrismaUI::InputHandler;

	SingleThreadExecutor uiThread;
	std::unique_ptr<RepeatingTaskRunner> logicRunner;
	NanoIdGenerator generator;
	std::atomic<bool> coreInitialized = false;

	RefPtr<Renderer> renderer;
	ID3D11Device* d3dDevice = nullptr;
	ID3D11DeviceContext* d3dContext = nullptr;
	HWND hWnd = nullptr;
	WNDPROC s_originalWndProc = nullptr;
	RE::BSGraphics::ScreenSize screenSize;

	std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
	std::unique_ptr<DirectX::CommonStates> commonStates;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorTexture;

	std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
	std::shared_mutex viewsMutex;

	std::map<std::pair<PrismaViewId, std::string>, JSCallbackData> PrismaUI::Core::jsCallbacks;
	std::mutex PrismaUI::Core::jsCallbacksMutex;

	inline REL::Relocation<Hooks::D3DPresentHook::D3DPresentFunc> RealD3dPresentFunc;

	PrismaView::~PrismaView() {
		ViewRenderer::ReleaseViewTexture(this);
	}

	void InitializeCoreSystem() {
		logger::info("Initializing PrismaUI Core System...");
		InitHooks();

		logicRunner = std::make_unique<RepeatingTaskRunner>([]() {
			uiThread.submit(&UpdateLogic).get();
			});

		uiThread.submit([] {
			try {
				Platform& plat = Platform::instance();
				plat.set_logger(new MyUltralightLogger());
				plat.set_font_loader(ultralight::GetPlatformFontLoader());

				plat.set_file_system(ultralight::GetPlatformFileSystem("."));

				Config config;
				plat.set_config(config);

				renderer = Renderer::Create();
				if (!renderer) {
					logger::critical("Failed to create Ultralight Renderer!");
				}
				else {
					logger::info("Ultralight Platform configured and Renderer created on UI thread.");
				}
			}
			catch (const std::exception& e) {
				logger::critical("Exception during Ultralight Platform/Renderer init on UI thread: {}", e.what());
			}
			catch (...) {
				logger::critical("Unknown exception during Ultralight Platform/Renderer init on UI thread.");
			}
			}).get();

		auto ui = RE::UI::GetSingleton();
		ui->Register(FocusMenu::MENU_NAME, FocusMenu::Creator);

		logger::info("PrismaUI Core System Initialized.");
	}

	void InitHooks() {
		logger::debug("Installing D3D Present hook...");
		RealD3dPresentFunc = Hooks::D3DPresentHook::Install(&D3DPresent);
		logger::info("D3D Present hook installed.");
	}

	void InitGraphics() {
		auto* renderManager = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderManager) {
			logger::critical("InitGraphics: RenderManager is null!"); return;
		}
		auto runtimeData = renderManager->GetRuntimeData();
		if (!d3dDevice) d3dDevice = reinterpret_cast<ID3D11Device*>(runtimeData.forwarder);
		if (!d3dContext) d3dContext = reinterpret_cast<ID3D11DeviceContext*>(runtimeData.context);

		if (!hWnd && runtimeData.renderWindows && runtimeData.renderWindows->hWnd) {
			hWnd = reinterpret_cast<HWND>(runtimeData.renderWindows->hWnd);
			screenSize = renderManager->GetScreenSize();
			if (!s_originalWndProc && hWnd) {
				s_originalWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)PrismaUI::InputHandler::HookedWndProc);
				if (s_originalWndProc) {
					logger::debug("WndProc hook installed via SetWindowLongPtr.");
					static std::atomic<bool> input_handler_initialized = false;
					bool expected_ih_init = false;

					if (input_handler_initialized.compare_exchange_strong(expected_ih_init, true)) {
						Initialize(hWnd, &uiThread, &views, &viewsMutex);
						SetOriginalWndProc(s_originalWndProc);
						logger::debug("PrismaUI::InputHandler initialized and original WndProc passed.");
					}
				}
				else {
					logger::error("Failed to install WndProc hook! GetLastError() = {}", GetLastError());
				}
			}
		}
		else if (!hWnd) {
			logger::warn("InitGraphics: Could not obtain HWND.");
		}

		if (d3dDevice && d3dContext) {
			if (!commonStates || !spriteBatch) {
				try {
					commonStates = std::make_unique<DirectX::CommonStates>(d3dDevice);
					spriteBatch = std::make_unique<DirectX::SpriteBatch>(d3dContext);
					logger::info("DirectXTK SpriteBatch and CommonStates (re)initialized.");
				}
				catch (const std::exception& e) {
					logger::critical("Failed to initialize DirectXTK: {}", e.what());
					commonStates.reset(); spriteBatch.reset();
				}
			}

			if (!cursorTexture && d3dDevice) {
				try {
					DirectX::CreateWICTextureFromFile(d3dDevice, L"Data/PrismaUI/misc/cursor.png", nullptr, &cursorTexture);
					logger::info("Cursor texture loaded successfully.");
				}
				catch (const std::exception& e) {
					logger::critical("Failed to load cursor texture: {}", e.what());
					cursorTexture.Reset();
				}
			}
		}
		else {
			logger::error("Cannot initialize DirectXTK: D3D device or context is null.");
			commonStates.reset(); spriteBatch.reset();
		}
	}

	void D3DPresent(uint32_t a_p1) {
		RealD3dPresentFunc(a_p1);

		if (!coreInitialized) return;

		if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0) {
			InitGraphics();
			if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0) return;
		}

		std::vector<PrismaViewId> viewsWithPendingRelease;
		{
			std::shared_lock lock(viewsMutex);
			for (const auto& pair : views) {
				if (pair.second && pair.second->pendingResourceRelease.load()) {
					viewsWithPendingRelease.push_back(pair.first);
				}
			}
		}

		for (const auto& viewId : viewsWithPendingRelease) {
			std::shared_ptr<PrismaView> viewData = nullptr;
			{
				std::shared_lock lock(viewsMutex);
				auto it = views.find(viewId);
				if (it != views.end()) {
					viewData = it->second;
				}
			}

			if (viewData) {
				logger::debug("D3DPresent: Releasing D3D resources for View [{}] from render thread", viewId);
				ViewRenderer::ReleaseViewTexture(viewData.get());
				viewData->pendingResourceRelease = false;
			}
		}

		uiThread.submit([dev = d3dDevice, ctx = d3dContext, hwnd = hWnd]() {
			if (!dev || !ctx || !hwnd || !renderer) return;

			std::vector<std::shared_ptr<PrismaView>> viewsToInitialize;
			{
				std::shared_lock lock(viewsMutex);
				for (auto& pair : views) {
					if (pair.second && !pair.second->ultralightView && !pair.second->htmlPathToLoad.empty()) {
						viewsToInitialize.push_back(pair.second);
					}
				}
			}

			for (auto& viewData : viewsToInitialize) {
				if (viewData->ultralightView) continue;

				logger::info("UI Thread: Creating View [{}] for path: {}", viewData->id, viewData->htmlPathToLoad);

				if (screenSize.width == 0 || screenSize.height == 0) {
					logger::error("UI Thread: Cannot create View [{}], screen size is zero.", viewData->id);
					continue;
				}

				ViewConfig view_config;
				view_config.is_accelerated = false;
				view_config.is_transparent = true;

				viewData->ultralightView = renderer->CreateView(screenSize.width, screenSize.height, view_config, nullptr);

				if (viewData->ultralightView) {
					viewData->loadListener = std::make_unique<Listeners::MyLoadListener>(viewData->id);
					viewData->viewListener = std::make_unique<Listeners::MyViewListener>(viewData->id);
					viewData->ultralightView->set_load_listener(viewData->loadListener.get());
					viewData->ultralightView->set_view_listener(viewData->viewListener.get());
					viewData->ultralightView->LoadURL(String(viewData->htmlPathToLoad.c_str()));
					viewData->ultralightView->Unfocus();
					viewData->htmlPathToLoad.clear();
					logger::info("UI Thread: View [{}] successfully created and loading URL.", viewData->id);
				}
				else {
					logger::error("UI Thread: Failed to create Ultralight View for ID [{}].", viewData->id);
					viewData->htmlPathToLoad = "[CREATION FAILED]";
				}
			}

			ProcessEvents();

			if (renderer) {
				renderer->RefreshDisplay(0);
				renderer->Render();
			}

			RenderViews();
			});

		std::vector<std::shared_ptr<PrismaView>> viewsToCheck;
		{
			std::shared_lock lock(viewsMutex);
			viewsToCheck.reserve(views.size());
			for (const auto& pair : views) {
				if (pair.second && pair.second->ultralightView) {
					viewsToCheck.push_back(pair.second);
				}
			}
		}

		for (const auto& viewData : viewsToCheck) {
			UpdateSingleTextureFromBuffer(viewData);
		}

		DrawViews();
		DrawCursor();
	}

	void Shutdown() {
		logger::info("Shutting down PrismaUI Core System...");

		std::vector<PrismaViewId> viewIdsToDestroy;
		{
			std::shared_lock lock(viewsMutex);
			for (const auto& pair : views) {
				viewIdsToDestroy.push_back(pair.first);
			}
		}

		for (const auto& id : viewIdsToDestroy) {
			try {
				ViewManager::Destroy(id);
			}
			catch (const std::exception& e) {
				logger::error("Error destroying view [{}] during shutdown: {}", id, e.what());
			}
		}

		cursorTexture.Reset();
		spriteBatch.reset();
		commonStates.reset();
		logger::debug("DirectXTK resources released.");

		if (s_originalWndProc && hWnd) {
			SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)s_originalWndProc);
			logger::info("WndProc hook removed.");
			s_originalWndProc = nullptr;
		}
		InputHandler::Shutdown();

		d3dDevice = nullptr;
		d3dContext = nullptr;
		hWnd = nullptr;

		if (logicRunner) {
			logicRunner->stop();
			logicRunner.reset();
		}

		{
			std::unique_lock lock(viewsMutex);
			views.clear();
		}
		if (renderer) {
			uiThread.submit([renderer_ptr = renderer]() mutable {
				logger::info("Releasing global renderer on UI thread.");
				renderer_ptr = nullptr;
				}).get();
			renderer = nullptr;
		}

		coreInitialized = false;
		logger::info("PrismaUI Core System shut down complete.");
	}
}
