#include <PrismaUI/PrismaUI.h>

namespace PrismaUI::Core {

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

	class MyLoadListener : public LoadListener {
		PrismaViewId viewId_;
	public:
		explicit MyLoadListener(PrismaViewId id) : viewId_(std::move(id)) {}
		virtual ~MyLoadListener() = default;

		virtual void OnBeginLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override {
			logger::info("View [{}]: LoadListener: Begin loading URL: {}", viewId_, url.utf8().data());
			uiThread.submit([id = viewId_] {
				std::shared_lock lock(viewsMutex);
				auto it = views.find(id);
				if (it != views.end()) {
					it->second->isLoadingFinished = false;
				}
				});
		}

		virtual void OnFinishLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override {
			logger::info("View [{}]: LoadListener: Finished loading URL: {}", viewId_, url.utf8().data());
			uiThread.submit([id = viewId_] {
				std::shared_lock lock(viewsMutex);
				auto it = views.find(id);
				if (it != views.end()) {
					it->second->isLoadingFinished = true;

					BindJSCallbacks(id);
				}
				});
		}

		virtual void OnFailLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url, const String& description, const String& error_domain, int error_code) override {
			logger::error("View [{}]: LoadListener: Failed loading URL: {}. Error: {}", viewId_, url.utf8().data(), description.utf8().data());
			uiThread.submit([id = viewId_] {
				std::shared_lock lock(viewsMutex);
				auto it = views.find(id);
				if (it != views.end()) {
					it->second->isLoadingFinished = false;
				}
				});
		}

		virtual void OnWindowObjectReady(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override {
			logger::info("View [{}]: LoadListener: Window object ready.", viewId_);
		}

		virtual void OnDOMReady(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override {
			logger::info("View [{}]: LoadListener: DOM ready.", viewId_);

			uiThread.submit([id = viewId_] {
				std::shared_lock lock(viewsMutex);
				auto it = views.find(id);
				if (it != views.end() && it->second->domReadyCallback) {
					it->second->domReadyCallback(id);
				}
				});
		}
	};

	class MyViewListener : public ViewListener {
		PrismaViewId viewId_;
	public:
		explicit MyViewListener(PrismaViewId id) : viewId_(std::move(id)) {}
		virtual ~MyViewListener() = default;

		virtual void OnAddConsoleMessage(View* caller, const ConsoleMessage& message) override {
			logger::info("View [{}]: JSConsole: {}", viewId_, message.message().utf8().data());
		}
	};

	class MyUltralightLogger : public Logger {
	public:
		virtual ~MyUltralightLogger() = default;
		virtual void LogMessage(LogLevel log_level, const String& message) override {
		}
	};

	PrismaView::~PrismaView() {
		ReleaseViewTexture(this);
	}

	PrismaViewId Create(const std::string& htmlPath, std::function<void(PrismaViewId)> onDomReadyCallback) {
		bool expected_init = false;
		if (coreInitialized.compare_exchange_strong(expected_init, true)) {
			InitializeCoreSystem();
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

		PrismaViewId newViewId = generator.generate();
		std::string fileUrl = "file:///Data/PrismaUI/views/" + htmlPath;

		auto viewData = std::make_shared<PrismaView>();
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

	void InitializeCoreSystem() {
		logger::info("Initializing PrismaUI Core System...");
		InitHooks();

		logicRunner = std::make_unique<RepeatingTaskRunner>([]() {
			uiThread.submit(&UpdateLogic);
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
				Destroy(id);
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

	void InitHooks() {
		logger::debug("Installing D3D Present hook...");
		RealD3dPresentFunc = Hooks::D3DPresentHook::Install(&D3DPresent);
		logger::info("D3D Present hook installed.");
	}

	void Show(const PrismaViewId& viewId) {
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

	void Hide(const PrismaViewId& viewId) {
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
			uiThread.submit([view_ptr = viewData->ultralightView]() {
				if (view_ptr) view_ptr->Unfocus();
				});
			logger::debug("View [{}] marked as Hidden and Unfocused.", viewId);
		}
		else {
			logger::warn("Hide: View ID [{}] not found.", viewId);
		}
	}

	bool IsHidden(const PrismaViewId& viewId) {
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end()) {
			return it->second->isHidden.load();
		}
		logger::warn("IsHidden: View ID [{}] not found.", viewId);
		return true;
	}

	bool IsValid(const PrismaViewId& viewId) {
		std::shared_lock lock(viewsMutex);
		return views.find(viewId) != views.end();
	}

	bool Focus(const PrismaViewId& viewId, bool pauseGame) {
		auto ui = RE::UI::GetSingleton();
		if (ui && ui->IsMenuOpen(RE::Console::MENU_NAME)) return false;

		std::shared_ptr<PrismaView> viewData = nullptr;
		{ std::shared_lock lock(viewsMutex); auto it = views.find(viewId); if (it != views.end()) viewData = it->second; }

		if (viewData && viewData->ultralightView) {
			if (viewData->isHidden.load()) {
				logger::warn("Focus: View [{}] is hidden, cannot focus.", viewId);
				return false;
			}

			std::vector<PrismaViewId> viewsToUnfocus;
			{
				std::shared_lock lock(viewsMutex);
				for (const auto& pair : views) {
					if (pair.first != viewId) {
						auto future = uiThread.submit([view_ptr = pair.second->ultralightView]() -> bool {
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

			uiThread.submit([view_ptr = viewData->ultralightView]() {
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

	void Unfocus(const PrismaViewId& viewId) {
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
			uiThread.submit([view_ptr = viewData->ultralightView]() {
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

	bool HasFocus(const PrismaViewId& viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (viewData) {
			auto future = uiThread.submit([view_ptr = viewData->ultralightView]() -> bool {
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

	bool ViewHasInputFocus(const PrismaViewId& viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (viewData && viewData->ultralightView) {
			auto future = uiThread.submit([view_ptr = viewData->ultralightView]() -> bool {
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

	void SetScrollingPixelSize(const PrismaViewId& viewId, int pixelSize) {
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

	int GetScrollingPixelSize(const PrismaViewId& viewId) {
		std::shared_lock lock(viewsMutex);
		auto it = views.find(viewId);
		if (it != views.end()) {
			return it->second->scrollingPixelSize;
		}
		logger::warn("GetScrollingPixelSize: View ID [{}] not found, returning default.", viewId);
		return 28;
	}

	JSValueRef InvokeCppCallback(JSContextRef ctx, JSObjectRef function,
		JSObjectRef thisObject, size_t argumentCount,
		const JSValueRef arguments[], JSValueRef* exception) {

		logger::debug("InvokeCppCallback: Called from JavaScript");

		JSValueRef dataValue = JSObjectGetProperty(ctx, function,
			JSStringCreateWithUTF8CString("data"), exception);

		if (!dataValue || JSValueIsNull(ctx, dataValue) || JSValueIsUndefined(ctx, dataValue)) {
			logger::error("InvokeCppCallback: No data attached to the function");
			return JSValueMakeUndefined(ctx);
		}

		JSStringRef viewIdKey = JSStringCreateWithUTF8CString("viewId");
		JSStringRef nameKey = JSStringCreateWithUTF8CString("name");

		JSObjectRef dataObj = JSValueToObject(ctx, dataValue, exception);
		if (!dataObj) {
			logger::error("InvokeCppCallback: Failed to convert data to object");
			JSStringRelease(viewIdKey);
			JSStringRelease(nameKey);
			return JSValueMakeUndefined(ctx);
		}

		JSValueRef viewIdValue = JSObjectGetProperty(ctx, dataObj, viewIdKey, exception);
		JSValueRef nameValue = JSObjectGetProperty(ctx, dataObj, nameKey, exception);

		JSStringRelease(viewIdKey);
		JSStringRelease(nameKey);

		if (!viewIdValue || !nameValue) {
			logger::error("InvokeCppCallback: Failed to get viewId or name from data object");
			return JSValueMakeUndefined(ctx);
		}

		JSStringRef viewIdStr = JSValueToStringCopy(ctx, viewIdValue, exception);
		JSStringRef nameStr = JSValueToStringCopy(ctx, nameValue, exception);

		if (!viewIdStr || !nameStr) {
			logger::error("InvokeCppCallback: Failed to convert viewId or name to string");
			if (viewIdStr) JSStringRelease(viewIdStr);
			if (nameStr) JSStringRelease(nameStr);
			return JSValueMakeUndefined(ctx);
		}

		size_t viewIdLen = JSStringGetMaximumUTF8CStringSize(viewIdStr);
		size_t nameLen = JSStringGetMaximumUTF8CStringSize(nameStr);

		std::vector<char> viewIdBuffer(viewIdLen);
		std::vector<char> nameBuffer(nameLen);

		JSStringGetUTF8CString(viewIdStr, viewIdBuffer.data(), viewIdLen);
		JSStringGetUTF8CString(nameStr, nameBuffer.data(), nameLen);

		JSStringRelease(viewIdStr);
		JSStringRelease(nameStr);

		PrismaViewId viewId = std::stoull(std::string(viewIdBuffer.data()));
		std::string name(nameBuffer.data());

		logger::debug("InvokeCppCallback: Looking for callback viewId={}, name={}", viewId, name);

		std::string paramStr;
		if (argumentCount > 0) {
			JSStringRef jsStrParam = JSValueToStringCopy(ctx, arguments[0], exception);
			if (jsStrParam) {
				size_t paramBufferSize = JSStringGetMaximumUTF8CStringSize(jsStrParam);
				std::vector<char> paramBuffer(paramBufferSize);
				JSStringGetUTF8CString(jsStrParam, paramBuffer.data(), paramBufferSize);
				JSStringRelease(jsStrParam);
				paramStr = paramBuffer.data();
			}
		}

		SimpleJSCallback targetCallback = nullptr;
		{
			std::lock_guard<std::mutex> lock(PrismaUI::Core::jsCallbacksMutex);
			auto it = PrismaUI::Core::jsCallbacks.find(std::make_pair(viewId, name));
			if (it != PrismaUI::Core::jsCallbacks.end()) {
				targetCallback = it->second.callback;
			}
		}

		if (targetCallback) {
			logger::debug("InvokeCppCallback: Found callback. Invoking with data: '{}'", paramStr);
			try {
				targetCallback(paramStr);
				logger::debug("InvokeCppCallback: Callback invoked successfully");
			}
			catch (const std::exception& e) {
				logger::error("InvokeCppCallback: Exception in callback: {}", e.what());
				if (exception) {
					JSStringRef errStr = JSStringCreateWithUTF8CString(e.what());
					*exception = JSValueMakeString(ctx, errStr);
					JSStringRelease(errStr);
				}
			}
			catch (...) {
				logger::error("InvokeCppCallback: Unknown exception in callback");
				if (exception) {
					JSStringRef errStr = JSStringCreateWithUTF8CString("Unknown error in C++ callback");
					*exception = JSValueMakeString(ctx, errStr);
					JSStringRelease(errStr);
				}
			}
		}
		else {
			logger::error("InvokeCppCallback: Callback not found for viewId={}, name={}", viewId, name);
			if (exception) {
				std::string errMsg = "C++ callback not found: " + name + " for view " + std::to_string(viewId);
				JSStringRef errorStr = JSStringCreateWithUTF8CString(errMsg.c_str());
				*exception = JSValueMakeString(ctx, errorStr);
				JSStringRelease(errorStr);
			}
		}

		return JSValueMakeUndefined(ctx);
	}

	void Invoke(const PrismaViewId& viewId, const String& script, std::function<void(std::string)> callback) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			logger::warn("Invoke: View ID [{}] not found.", viewId);
			if (callback) {
				callback("");
			}
			return;
		}

		uiThread.submit([view_ptr = viewData->ultralightView, script_copy = script, callback]() {
			String result = "";
			if (view_ptr) {
				try {
					result = view_ptr->EvaluateScript(script_copy);
				}
				catch (const std::exception& e) {
					logger::error("Exception during EvaluateScript: {}", e.what());
				}
				catch (...) {
					logger::error("Unknown exception during EvaluateScript");
				}
			}

			if (callback) {
				callback(result.utf8().data());
			}
			});
	}

	void InteropCall(const PrismaViewId& viewId, const std::string& functionName, const std::string& argument) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			logger::warn("InteropCall: View ID [{}] not found.", viewId);
			return;
		}

		if (!viewData->ultralightView) {
			logger::warn("InteropCall: View ID [{}] has no Ultralight view object.", viewId);
			return;
		}

		uiThread.submit([view_ptr = viewData->ultralightView, funcName = functionName, arg = argument, viewId]() {
			if (!view_ptr) {
				return;
			}

			auto scoped_context = view_ptr->LockJSContext();
			JSContextRef ctx = (*scoped_context);
			JSValueRef exception = nullptr;

			JSObjectRef globalObj = JSContextGetGlobalObject(ctx);

			JSRetainPtr<JSStringRef> funcNameStr = adopt(JSStringCreateWithUTF8CString(funcName.c_str()));
			JSValueRef funcValue = JSObjectGetProperty(ctx, globalObj, funcNameStr.get(), &exception);

			if (exception) {
				JSStringRef exceptionStr = JSValueToStringCopy(ctx, exception, nullptr);
				size_t bufferSize = JSStringGetMaximumUTF8CStringSize(exceptionStr);
				std::vector<char> buffer(bufferSize);
				JSStringGetUTF8CString(exceptionStr, buffer.data(), bufferSize);
				logger::error("InteropCall [{}]: Exception getting function '{}': {}", viewId, funcName, buffer.data());
				JSStringRelease(exceptionStr);
				return;
			}

			if (JSValueIsObject(ctx, funcValue)) {
				JSObjectRef funcObj = JSValueToObject(ctx, funcValue, nullptr);

				if (funcObj && JSObjectIsFunction(ctx, funcObj)) {
					JSRetainPtr<JSStringRef> argStr = adopt(JSStringCreateWithUTF8CString(arg.c_str()));
					const JSValueRef args[] = { JSValueMakeString(ctx, argStr.get()) };

					JSObjectCallAsFunction(ctx, funcObj, globalObj, 1, args, &exception);

					if (exception) {
						JSStringRef exceptionStr = JSValueToStringCopy(ctx, exception, nullptr);
						size_t bufferSize = JSStringGetMaximumUTF8CStringSize(exceptionStr);
						std::vector<char> buffer(bufferSize);
						JSStringGetUTF8CString(exceptionStr, buffer.data(), bufferSize);
						logger::error("InteropCall [{}]: Exception calling function '{}': {}", viewId, funcName, buffer.data());
						JSStringRelease(exceptionStr);
					}
				}
				else {
					logger::warn("InteropCall [{}]: Global property '{}' is not a function.", viewId, funcName);
				}
			}
			else {
				logger::warn("InteropCall [{}]: Global property '{}' not found or not an object.", viewId, funcName);
			}
			});
	}

	void RegisterJSListener(const PrismaViewId& viewId, const std::string& name, SimpleJSCallback callback) {
		if (!IsValid(viewId)) {
			logger::error("RegisterJSListener: View ID [{}] not found.", viewId);
			return;
		}

		{
			std::lock_guard<std::mutex> lock(jsCallbacksMutex);
			JSCallbackData data;
			data.viewId = viewId;
			data.name = name;
			data.callback = callback;
			jsCallbacks[std::make_pair(viewId, name)] = std::move(data);
			logger::debug("RegisterJSListener: Registered callback '{}' for view [{}]", name, viewId);
		}

		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (viewData && viewData->ultralightView && viewData->isLoadingFinished) {
			uiThread.submit([viewId, name]() {
				BindJSCallbacks(viewId);
				});
		}
	}

	void BindJSCallbacks(const PrismaViewId& viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData || !viewData->ultralightView || !viewData->isLoadingFinished) {
			logger::warn("BindJSCallbacks: View [{}] not ready or not loaded.", viewId);
			return;
		}

		std::vector<JSCallbackData> viewCallbacks;
		{
			std::lock_guard<std::mutex> lock(jsCallbacksMutex);
			for (const auto& pair : jsCallbacks) {
				if (pair.first.first == viewId) {
					viewCallbacks.push_back(pair.second);
				}
			}
		}

		if (viewCallbacks.empty()) {
			return;
		}

		auto scoped_context = viewData->ultralightView->LockJSContext();
		JSContextRef ctx = (*scoped_context);
		JSObjectRef globalObj = JSContextGetGlobalObject(ctx);

		for (const auto& callbackData : viewCallbacks) {
			logger::debug("BindJSCallbacks: Binding callback '{}' for view [{}]", callbackData.name, callbackData.viewId);

			JSObjectRef dataObj = JSObjectMake(ctx, nullptr, nullptr);

			JSStringRef viewIdKey = JSStringCreateWithUTF8CString("viewId");
			JSStringRef nameKey = JSStringCreateWithUTF8CString("name");

			JSStringRef viewIdValue = JSStringCreateWithUTF8CString(std::to_string(callbackData.viewId).c_str());
			JSStringRef nameValue = JSStringCreateWithUTF8CString(callbackData.name.c_str());

			JSObjectSetProperty(ctx, dataObj, viewIdKey, JSValueMakeString(ctx, viewIdValue),
				kJSPropertyAttributeReadOnly, nullptr);
			JSObjectSetProperty(ctx, dataObj, nameKey, JSValueMakeString(ctx, nameValue),
				kJSPropertyAttributeReadOnly, nullptr);

			JSStringRelease(viewIdKey);
			JSStringRelease(nameKey);
			JSStringRelease(viewIdValue);
			JSStringRelease(nameValue);

			JSStringRef funcJS = JSStringCreateWithUTF8CString(callbackData.name.c_str());

			JSObjectRef funcObj = JSObjectMakeFunctionWithCallback(ctx, funcJS, InvokeCppCallback);

			JSStringRef dataKey = JSStringCreateWithUTF8CString("data");
			JSObjectSetProperty(ctx, funcObj, dataKey, dataObj, kJSPropertyAttributeReadOnly, nullptr);
			JSStringRelease(dataKey);

			JSObjectSetProperty(ctx, globalObj, funcJS, funcObj, kJSPropertyAttributeNone, nullptr);

			JSStringRelease(funcJS);

			logger::debug("BindJSCallbacks: Successfully bound callback '{}' for view [{}]",
				callbackData.name, callbackData.viewId);
		}
	}

	JSValueRef JSCallbackDispatcher(JSContextRef ctx, JSObjectRef function,
		JSObjectRef thisObject, size_t argumentCount,
		const JSValueRef arguments[], JSValueRef* exception) {

		logger::debug("JSCallbackDispatcher: Entered.");

		JSCallbackData* callbackDataPtr = static_cast<JSCallbackData*>(JSObjectGetPrivate(function));

		if (!callbackDataPtr) {
			logger::error("JSCallbackDispatcher: Failed to get private data (C++ JSCallbackData*) from function object.");
			if (exception) {
				JSStringRef errorStr = JSStringCreateWithUTF8CString("Internal C++ error: private data (callback ptr) missing for JS callback.");
				*exception = JSValueMakeString(ctx, errorStr);
				JSStringRelease(errorStr);
			}
			return JSValueMakeNull(ctx);
		}
		logger::debug("JSCallbackDispatcher: Private data (C++ JSCallbackData*) retrieved. Name: '{}', ViewID: '{}'", callbackDataPtr->name, callbackDataPtr->viewId);

		PrismaViewId retrievedViewId = callbackDataPtr->viewId;
		std::string retrievedName = callbackDataPtr->name;
		SimpleJSCallback targetCallback = callbackDataPtr->callback;

		logger::debug("JSCallbackDispatcher: Retrieved from C++ pointer -> View ID: '{}', Name: '{}'", retrievedViewId, retrievedName);

		std::string paramStrData;
		if (argumentCount > 0) {
			if (JSValueIsString(ctx, arguments[0])) {
				JSStringRef jsStrParam = JSValueToStringCopy(ctx, arguments[0], exception);
				if (jsStrParam) {
					size_t paramBufferSize = JSStringGetMaximumUTF8CStringSize(jsStrParam);
					std::vector<char> paramBuffer(paramBufferSize);
					JSStringGetUTF8CString(jsStrParam, paramBuffer.data(), paramBufferSize);
					JSStringRelease(jsStrParam);
					paramStrData = paramBuffer.data();
					logger::debug("JSCallbackDispatcher: Arg 0 (string): '{}'", paramStrData);
				}
				else {
					logger::warn("JSCallbackDispatcher: Arg 0 was not convertible to string (JSValueToStringCopy failed).");
					if (exception && (!(*exception) || JSValueIsNull(ctx, *exception))) {
						JSStringRef errorStr = JSStringCreateWithUTF8CString("C++ callback expected a string argument, but conversion failed.");
						*exception = JSValueMakeString(ctx, errorStr);
						JSStringRelease(errorStr);
					}
				}
			}
			else {
				logger::warn("JSCallbackDispatcher: Arg 0 passed from JS was not a string type.");
				if (exception) {
					JSStringRef errorStr = JSStringCreateWithUTF8CString("C++ callback expected a string argument, but received a different type.");
					*exception = JSValueMakeString(ctx, errorStr);
					JSStringRelease(errorStr);
				}
			}
		}
		else {
			logger::debug("JSCallbackDispatcher: No arguments passed from JS. Expected 1 string argument.");
		}

		if (targetCallback) {
			logger::debug("JSCallbackDispatcher: Target callback found. Invoking with data: '{}'", paramStrData);
			try {
				targetCallback(paramStrData);
				logger::debug("JSCallbackDispatcher: Target callback invoked successfully.");
			}
			catch (const std::exception& e) {
				logger::error("JSCallbackDispatcher: C++ exception in registered callback for '{}'/'{}': {}", retrievedName, retrievedViewId, e.what());
				if (exception) {
					JSStringRef errorStr = JSStringCreateWithUTF8CString(e.what());
					*exception = JSValueMakeString(ctx, errorStr);
					JSStringRelease(errorStr);
				}
			}
			catch (...) {
				logger::error("JSCallbackDispatcher: Unknown C++ exception in registered callback for '{}'/'{}'", retrievedName, retrievedViewId);
				if (exception) {
					JSStringRef errorStr = JSStringCreateWithUTF8CString("Unknown C++ error in JS callback.");
					*exception = JSValueMakeString(ctx, errorStr);
					JSStringRelease(errorStr);
				}
			}
		}
		else {
			logger::warn("JSCallbackDispatcher: Target callback was null within JSCallbackData for View ID: '{}', Name: '{}'", retrievedViewId, retrievedName);
			if (exception) {
				std::string errMsg = "Internal C++ error: Callback function pointer was null for " + retrievedName;
				JSStringRef errorStr = JSStringCreateWithUTF8CString(errMsg.c_str());
				*exception = JSValueMakeString(ctx, errorStr);
				JSStringRelease(errorStr);
			}
		}
		logger::debug("JSCallbackDispatcher: Exiting.");
		return JSValueMakeNull(ctx);
	}

	void Destroy(const PrismaViewId& viewId) {
		logger::info("Destroy: Beginning destruction of View [{}]", viewId);

		if (!IsValid(viewId)) {
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
		auto ultralightCleanupFuture = uiThread.submit([viewId, viewData = viewDataToDestroy]() {
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
				s_originalWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)InputHandler::HookedWndProc);
				if (s_originalWndProc) {
					logger::debug("WndProc hook installed via SetWindowLongPtr.");
					static std::atomic<bool> input_handler_initialized = false;
					bool expected_ih_init = false;

					if (input_handler_initialized.compare_exchange_strong(expected_ih_init, true)) {
						PrismaUI::InputHandler::Initialize(hWnd, &uiThread, &views, &viewsMutex);
						PrismaUI::InputHandler::SetOriginalWndProc(s_originalWndProc);
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

	void UpdateLogic() {
		if (renderer) {
			renderer->Update();
		}
	}

	void RenderViews() {
		if (!renderer) return;

		std::vector<std::shared_ptr<PrismaView>> viewsToRender;
		{
			std::shared_lock lock(viewsMutex);
			viewsToRender.reserve(views.size());
			for (const auto& pair : views) {
				if (pair.second && !pair.second->isHidden) {
					viewsToRender.push_back(pair.second);
				}
			}
		}

		for (const auto& viewData : viewsToRender) {
			RenderSingleView(viewData);
		}
	}

	void RenderSingleView(std::shared_ptr<PrismaView> viewData) {
		if (!viewData || !viewData->ultralightView) return;

		Surface* surface_base = viewData->ultralightView->surface();
		if (!surface_base) return;

		BitmapSurface* surface = static_cast<BitmapSurface*>(surface_base);

		if (viewData->isLoadingFinished && !surface->dirty_bounds().IsEmpty()) {
			CopyBitmapToBuffer(viewData);
			surface->ClearDirtyBounds();
		}
	}

	void CopyBitmapToBuffer(std::shared_ptr<PrismaView> viewData) {
		if (!viewData || !viewData->ultralightView) return;
		BitmapSurface* surface = static_cast<BitmapSurface*>(viewData->ultralightView->surface());
		if (!surface) return;
		RefPtr<Bitmap> bitmap = surface->bitmap();
		if (!bitmap) return;

		void* pixels = bitmap->LockPixels();
		if (!pixels) { logger::error("View [{}]: Failed to lock bitmap pixels.", viewData->id); return; }

		uint32_t width = bitmap->width();
		uint32_t height = bitmap->height();
		uint32_t stride = bitmap->row_bytes();
		size_t required_size = static_cast<size_t>(height) * stride;
		if (width == 0 || height == 0 || required_size == 0) {
			bitmap->UnlockPixels(); return;
		}

		bool success = false;
		{
			std::lock_guard lock(viewData->bufferMutex);
			try {
				if (viewData->pixelBuffer.size() != required_size) { viewData->pixelBuffer.resize(required_size); }
				memcpy(viewData->pixelBuffer.data(), pixels, required_size);
				viewData->bufferWidth = width; viewData->bufferHeight = height; viewData->bufferStride = stride;
				success = true;
			}
			catch (const std::exception& e) {
				logger::error("View [{}]: Exception during pixel buffer copy/resize: {}", viewData->id, e.what());
				viewData->pixelBuffer.clear(); viewData->pixelBuffer.shrink_to_fit();
				viewData->bufferWidth = viewData->bufferHeight = viewData->bufferStride = 0;
			}
		}

		bitmap->UnlockPixels();
		if (success) viewData->newFrameReady = true;
		else viewData->newFrameReady = false;
	}

	void ReleaseViewTexture(PrismaView* viewData) {
		if (!viewData) return;

		if (viewData->textureView) { viewData->textureView->Release(); viewData->textureView = nullptr; }
		if (viewData->texture) { viewData->texture->Release(); viewData->texture = nullptr; }
		viewData->textureWidth = 0; viewData->textureHeight = 0;
	}

	void UpdateSingleTextureFromBuffer(std::shared_ptr<PrismaView> viewData) {
		if (!viewData) return;

		if (viewData->pendingResourceRelease.load()) {
			logger::debug("UpdateSingleTextureFromBuffer: Releasing D3D resources for View [{}] based on pendingResourceRelease flag", viewData->id);

			ReleaseViewTexture(viewData.get());

			viewData->pendingResourceRelease = false;
			return;
		}

		bool expected = true;
		if (!viewData->newFrameReady.compare_exchange_strong(expected, false)) {
			return;
		}

		std::lock_guard lock(viewData->bufferMutex);
		if (viewData->pixelBuffer.empty() || viewData->bufferWidth == 0 || viewData->bufferHeight == 0) {
			return;
		}

		CopyPixelsToTexture(viewData.get(), viewData->pixelBuffer.data(),
			viewData->bufferWidth, viewData->bufferHeight,
			viewData->bufferStride);
	}

	void CopyPixelsToTexture(PrismaView* viewData, void* pixels, uint32_t width, uint32_t height, uint32_t stride) {
		if (!viewData || !d3dDevice || !d3dContext || !pixels || width == 0 || height == 0) return;

		if (!viewData->texture || viewData->textureWidth != width || viewData->textureHeight != height) {
			logger::debug("View [{}]: Creating/Recreating texture ({}x{})", viewData->id, width, height);
			ReleaseViewTexture(viewData);
			D3D11_TEXTURE2D_DESC desc;
			ZeroMemory(&desc, sizeof(desc));
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &viewData->texture);

			if (FAILED(hr)) {
				logger::critical("View [{}]: Failed to create texture! HR={:#X}", viewData->id, hr);
				ReleaseViewTexture(viewData); return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc; ZeroMemory(&srvDesc, sizeof(srvDesc));

			srvDesc.Format = desc.Format; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;

			hr = d3dDevice->CreateShaderResourceView(viewData->texture, &srvDesc, &viewData->textureView);

			if (FAILED(hr)) {
				logger::critical("View [{}]: Failed to create SRV! HR={:#X}", viewData->id, hr); ReleaseViewTexture(viewData);
				return;
			}

			viewData->textureWidth = width;
			viewData->textureHeight = height;
			logger::debug("View [{}]: Texture/SRV created/resized.", viewData->id);
		}

		D3D11_MAPPED_SUBRESOURCE mappedResource;
		HRESULT hr = d3dContext->Map(viewData->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		if (FAILED(hr)) {
			logger::error("View [{}]: Failed to map texture! HR={:#X}", viewData->id, hr);
			return;
		}

		std::byte* source = static_cast<std::byte*>(pixels);
		std::byte* dest = static_cast<std::byte*>(mappedResource.pData);
		uint32_t destPitch = mappedResource.RowPitch;

		if (destPitch == stride) {
			memcpy(dest, source, (size_t)height * stride);
		}
		else {
			for (uint32_t y = 0; y < height; ++y) memcpy(dest + y * destPitch, source + y * stride, stride);
		}

		d3dContext->Unmap(viewData->texture, 0);
	}

	void DrawCursor() {
		if (!spriteBatch || !commonStates || !cursorTexture) {
			return;
		}

		if (!PrismaUI::InputHandler::IsAnyInputCaptureActive()) {
            return;
        }

		auto cursor = RE::MenuCursor::GetSingleton();
		if (!cursor) {
			return;
		}

		ID3D11BlendState* backupBlendState = nullptr;
		FLOAT backupBlendFactor[4];
		UINT backupSampleMask = 0;
		ID3D11DepthStencilState* backupDepthStencilState = nullptr;
		UINT backupStencilRef = 0;
		ID3D11RasterizerState* backupRasterizerState = nullptr;

		d3dContext->OMGetBlendState(&backupBlendState, backupBlendFactor, &backupSampleMask);
		d3dContext->OMGetDepthStencilState(&backupDepthStencilState, &backupStencilRef);
		d3dContext->RSGetState(&backupRasterizerState);

		spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());

		DirectX::SimpleMath::Vector2 position(cursor->cursorPosX, cursor->cursorPosY);
		spriteBatch->Draw(cursorTexture.Get(), position);

		spriteBatch->End();

		d3dContext->OMSetBlendState(backupBlendState, backupBlendFactor, backupSampleMask);
		d3dContext->OMSetDepthStencilState(backupDepthStencilState, backupStencilRef);
		d3dContext->RSSetState(backupRasterizerState);

		if (backupBlendState) backupBlendState->Release();
		if (backupDepthStencilState) backupDepthStencilState->Release();
		if (backupRasterizerState) backupRasterizerState->Release();
	}

	void DrawViews() {
		if (!spriteBatch || !commonStates) return;

		std::vector<std::shared_ptr<PrismaView>> viewsToDraw;
		{
			std::shared_lock lock(viewsMutex);
			viewsToDraw.reserve(views.size());
			for (const auto& pair : views) {
				if (pair.second && !pair.second->isHidden.load() &&
					!pair.second->pendingResourceRelease.load() &&
					pair.second->textureView) {
					viewsToDraw.push_back(pair.second);
				}
			}
		}

		if (viewsToDraw.empty()) return;

		try {
			ID3D11BlendState* backupBlendState = nullptr; FLOAT backupBlendFactor[4]; UINT backupSampleMask = 0;
			ID3D11DepthStencilState* backupDepthStencilState = nullptr; UINT backupStencilRef = 0;
			ID3D11RasterizerState* backupRasterizerState = nullptr;
			d3dContext->OMGetBlendState(&backupBlendState, backupBlendFactor, &backupSampleMask);
			d3dContext->OMGetDepthStencilState(&backupDepthStencilState, &backupStencilRef);
			d3dContext->RSGetState(&backupRasterizerState);

			spriteBatch->Begin(DirectX::SpriteSortMode_Deferred, commonStates->AlphaBlend());

			for (const auto& viewData : viewsToDraw) {
				DrawSingleTexture(viewData);
			}

			spriteBatch->End();

			d3dContext->OMSetBlendState(backupBlendState, backupBlendFactor, backupSampleMask);
			d3dContext->OMSetDepthStencilState(backupDepthStencilState, backupStencilRef);
			d3dContext->RSSetState(backupRasterizerState);
			if (backupBlendState) backupBlendState->Release();
			if (backupDepthStencilState) backupDepthStencilState->Release();
			if (backupRasterizerState) backupRasterizerState->Release();

		}
		catch (const std::exception& e) {
			logger::error("Error during SpriteBatch drawing loop: {}", e.what());
		}
		catch (...) {
			logger::error("Unknown error during SpriteBatch drawing loop.");
		}
	}

	void DrawSingleTexture(std::shared_ptr<PrismaView> viewData) {
		if (!viewData || !viewData->textureView || viewData->textureWidth == 0 || viewData->textureHeight == 0) return;

		DirectX::SimpleMath::Vector2 position(0.0f, 0.0f);
		RECT sourceRect = { 0, 0, (long)viewData->textureWidth, (long)viewData->textureHeight };

		spriteBatch->Draw(
			viewData->textureView, position, &sourceRect,
			DirectX::Colors::White, 0.f, DirectX::SimpleMath::Vector2::Zero,
			1.0f, DirectX::SpriteEffects_None, 0.f
		);
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
				ReleaseViewTexture(viewData.get());
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
					viewData->loadListener = std::make_unique<MyLoadListener>(viewData->id);
					viewData->viewListener = std::make_unique<MyViewListener>(viewData->id);
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

			PrismaUI::InputHandler::ProcessEvents();

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

} // namespace PrismaUI::Core
