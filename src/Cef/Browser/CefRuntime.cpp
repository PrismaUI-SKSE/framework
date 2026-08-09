#include "Cef/Shared/Constants.h"
#include "PCH.h"

#ifdef GetNextSibling
    #undef GetNextSibling
#endif

#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "Cef/Browser/BrowserCefUtils.h"
#include "Cef/Browser/CefOsrClient.h"
#include "Cef/Browser/CefRuntime.h"
#include "Cef/Browser/OverlayTexture.h"
#include "Cef/Shared/BrowserToRendererMessages.h"
#include "Cef/Shared/RendererToBrowserMessages.h"
#include "Cef/Shared/ViewUtils.h"
#include "Cef/Subprocess/PrismaCefApp.h"
#include "PrismaUI/Communication.h"
#include "Utils/DllLoader.h"
#include "Utils/VariantUtils.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"

namespace {
    constexpr std::chrono::milliseconds kBrowserCloseTimeout{5000};
    std::wstring ToFileUrl(const std::filesystem::path& path) {
        std::wstring generic = std::filesystem::absolute(path).generic_wstring();
        if (!generic.empty() && generic.front() != L'/') {
            generic.insert(generic.begin(), L'/');
        }
        return L"file://" + generic;
    }

    std::string NarrowAscii(const std::wstring& value) {
        std::string result;
        result.reserve(value.size());
        for (const wchar_t ch : value) {
            result.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : '?');
        }
        return result;
    }

    // Derives the "<HighPart>,<LowPart>" decimal LUID of the DXGI adapter backing
    // `device` so Chromium's GPU process can be pinned to the same adapter Skyrim
    // renders on (use-adapter-luid). Returns an empty string when the adapter LUID
    // cannot be resolved, leaving CEF on its default GPU selection.
    std::string BuildAdapterLuidSwitch(ID3D11Device* device) {
        if (!device) {
            logger::warn("CEF GPU adapter not pinned: render device is null.");
            return {};
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
        if (FAILED(hr)) {
            logger::warn("CEF GPU adapter not pinned: ID3D11Device exposes no IDXGIDevice. HR={:#X}",
                         static_cast<unsigned int>(hr));
            return {};
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
        if (FAILED(hr)) {
            logger::warn("CEF GPU adapter not pinned: IDXGIDevice::GetAdapter failed. HR={:#X}",
                         static_cast<unsigned int>(hr));
            return {};
        }

        DXGI_ADAPTER_DESC desc{};
        hr = adapter->GetDesc(&desc);
        if (FAILED(hr)) {
            logger::warn("CEF GPU adapter not pinned: IDXGIAdapter::GetDesc failed. HR={:#X}",
                         static_cast<unsigned int>(hr));
            return {};
        }

        std::string value = std::to_string(desc.AdapterLuid.HighPart) + "," + std::to_string(desc.AdapterLuid.LowPart);
        logger::info("Pinning CEF GPU process to Skyrim render adapter '{}' (LUID {}).", NarrowAscii(desc.Description),
                     value);
        return value;
    }

    std::string MakeIframeName(uint64_t viewId) { return std::to_string(viewId); }

    bool StartsWithInsensitive(std::string_view value, std::string_view prefix) {
        if (value.size() < prefix.size()) {
            return false;
        }

        for (size_t i = 0; i < prefix.size(); ++i) {
            const char lhs = value[i];
            const char rhs = prefix[i];
            if (lhs >= 'A' && lhs <= 'Z') {
                if (static_cast<char>(lhs - 'A' + 'a') != rhs) {
                    return false;
                }
            } else if (lhs != rhs) {
                return false;
            }
        }
        return true;
    }

    bool IsAbsoluteBrowserUrl(std::string_view value) {
        return StartsWithInsensitive(value, "http://") || StartsWithInsensitive(value, "https://") ||
               StartsWithInsensitive(value, "file://") || StartsWithInsensitive(value, "about:") ||
               StartsWithInsensitive(value, "data:");
    }

    std::string ResolveViewUrl(std::string_view urlOrPath) {
        if (IsAbsoluteBrowserUrl(urlOrPath)) {
            return std::string(urlOrPath);
        }

        std::filesystem::path path{std::string(urlOrPath)};
        if (path.is_relative()) {
            path = PrismaUI::Utils::GetBasePath() / "views" / path;
        }
        return NarrowAscii(ToFileUrl(path));
    }

    std::string JsLiteralString(std::string_view value) {
        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');
        for (const unsigned char ch : value) {
            switch (ch) {
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\b':
                    escaped += "\\b";
                    break;
                case '\f':
                    escaped += "\\f";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    if (ch < 0x20) {
                        constexpr char kHex[] = "0123456789abcdef";
                        escaped += "\\u00";
                        escaped.push_back(kHex[(ch >> 4) & 0x0F]);
                        escaped.push_back(kHex[ch & 0x0F]);
                    } else {
                        escaped.push_back(static_cast<char>(ch));
                    }
                    break;
            }
        }
        escaped.push_back('"');
        return escaped;
    }

    class DevToolsClient final : public CefClient, public CefLifeSpanHandler {
    public:
        using BrowserCallback = std::function<void(CefRefPtr<CefBrowser>)>;

        DevToolsClient(BrowserCallback onCreated, BrowserCallback onClosed)
            : onCreated_(std::move(onCreated)), onClosed_(std::move(onClosed)) {}

        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
            CEF_REQUIRE_UI_THREAD();
            if (browser) {
                logger::info("CEF DevTools OnAfterCreated browser [{}].", browser->GetIdentifier());
            } else {
                logger::error("CEF DevTools OnAfterCreated received a null browser.");
            }
            if (onCreated_) {
                onCreated_(browser);
            }
        }

        bool DoClose(CefRefPtr<CefBrowser> browser) override {
            CEF_REQUIRE_UI_THREAD();
            logger::info("CEF DevTools DoClose browser [{}].", browser ? browser->GetIdentifier() : -1);
            return false;
        }

        void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
            CEF_REQUIRE_UI_THREAD();
            logger::info("CEF DevTools OnBeforeClose browser [{}].", browser ? browser->GetIdentifier() : -1);
            if (onClosed_) {
                onClosed_(browser);
            }
        }

    private:
        BrowserCallback onCreated_;
        BrowserCallback onClosed_;

        IMPLEMENT_REFCOUNTING(DevToolsClient);
    };
}

namespace PrismaUI::Cef {
    struct InitState {
    public:
        bool WaitForInitialize() const {
            _state.wait(0);
            return _state == 1;
        }

        void SetInitialized() {
            _state.store(1, std::memory_order_release);
            _state.notify_one();
        }

        void SetFailed() {
            _state.store(2, std::memory_order_release);
            _state.notify_one();
        }

        void Reset() { _state.store(0, std::memory_order_release); }

    private:
        std::atomic<int> _state{0};
    };

    struct CefRuntime::Impl {
        mutable std::mutex stateMutex;
        CefRefPtr<CefApp> app;
        CefRefPtr<CefOsrClient> client;
        std::atomic<bool> initializeAttempted = false;
        std::atomic<bool> initialized = false;
        std::atomic<bool> shuttingDown = false;
        std::atomic<bool> shutdownSkipped = false;
        uint32_t width = 0;
        uint32_t height = 0;
        HWND hwnd = nullptr;
        mutable std::mutex devToolsMutex;
        CefRefPtr<CefClient> devToolsClient;
        CefRefPtr<CefBrowser> devToolsBrowser;
        std::atomic<bool> devToolsOpen = false;
        std::atomic<int> devToolsTargetBrowserId = -1;
        std::atomic<int> devToolsBrowserId = -1;
        InitState initState{};

        struct ShellViewState {
            uint64_t viewId = 0;
            std::string iframeName;
            std::string resolvedUrl;
            bool hidden = false;
            bool focused = false;
            int order = 0;
            std::string loadState = "not-created";
            std::string lastFrameIdentifier;
            std::string lastFrameName;
        };

        mutable std::mutex shellMutex;
        std::map<uint64_t, ShellViewState> shellViews;
        bool shellReady = false;
        std::string shellFrameIdentifier;
        std::string shellUrl;

        OverlayTexture overlay;

        struct InvokeEntry {
            uint64_t viewId = 0;
            std::function<void(std::string)> callback;
        };
        std::mutex invokeMutex;
        std::atomic<uint64_t> nextRequestId = 1;
        std::map<uint64_t, InvokeEntry> pendingInvokes;
    };

    CefRuntime::CefRuntime() : _impl(std::make_unique<Impl>()) {}

    CefRuntime::~CefRuntime() = default;

    CefRuntime& CefRuntime::GetSingleton() {
        static CefRuntime instance;
        return instance;
    }

    bool CefRuntime::Initialize(HWND hwnd, uint32_t width, uint32_t height, ID3D11Device* renderDevice,
                                ID3D11DeviceContext* context) const {
        std::lock_guard lock(_impl->stateMutex);

        if (_impl->initialized.load(std::memory_order_acquire)) {
            return true;
        }

        if (_impl->initializeAttempted.exchange(true, std::memory_order_acq_rel)) {
            logger::debug("CEF initialization was already attempted and did not complete successfully.");
            return false;
        }

        logger::info("Initializing CEF runtime for PrismaUI shell browser.");

        if (!Utils::DllLoader::GetSingleton().LoadCefLibraries()) {
            logger::error("CEF runtime initialization failed while preparing CEF libraries.");
            return false;
        }

        const std::filesystem::path basePath = Utils::GetBasePath();
        const std::filesystem::path libsPath = basePath / "libs";
        const std::filesystem::path subprocessPath = libsPath / "PrismaUICefSubprocess.exe";
        const std::filesystem::path resourcesPath = libsPath;
        const std::filesystem::path localesPath = libsPath / "locales";
        const std::filesystem::path logsPath = basePath / "logs";
        const std::filesystem::path logFile = logsPath / "cef.log";
        const std::filesystem::path shellPath = basePath / "shell" / "index.html";
        const std::wstring shellUrl = ToFileUrl(shellPath);
        const std::string shellUrlLog = NarrowAscii(shellUrl);

        std::error_code error;
        std::filesystem::create_directories(logsPath, error);
        if (error) {
            logger::error("Failed to create CEF log directory '{}': {}", logsPath.string(), error.message());
            return false;
        }

        if (!std::filesystem::exists(shellPath)) {
            logger::warn("CEF shell smoke document does not exist yet: {}", shellPath.string());
        }

        logger::info("CEF subprocess path: {}", subprocessPath.string());
        logger::info("CEF resources path: {}", resourcesPath.string());
        logger::info("CEF locales path: {}", localesPath.string());
        logger::info("CEF log file: {}", logFile.string());
        logger::info("CEF shell URL: {}", shellUrlLog);
        logger::info("CEF message loop mode: multi_threaded_message_loop=true");

        CefMainArgs mainArgs(GetModuleHandleW(nullptr));
        CefSettings settings;
        settings.no_sandbox = true;
        settings.windowless_rendering_enabled = true;
        settings.multi_threaded_message_loop = true;
        settings.log_severity = LOGSEVERITY_INFO;
        CefString(&settings.browser_subprocess_path).FromWString(subprocessPath.wstring());
        CefString(&settings.resources_dir_path).FromWString(resourcesPath.wstring());
        CefString(&settings.locales_dir_path).FromWString(localesPath.wstring());
        CefString(&settings.log_file).FromWString(logFile.wstring());
        CefString(&settings.locale).FromASCII("en-US");

        _impl->app = CreatePrismaCefApp(BuildAdapterLuidSwitch(renderDevice));
        logger::info("Calling CefInitialize.");
        if (!CefInitialize(mainArgs, settings, _impl->app, nullptr)) {
            auto errorCode = CefGetExitCode();
            logger::error("CefInitialize failed, exit code: {}", errorCode);
            _impl->app = nullptr;
            return false;
        }

        if (!_impl->overlay.Initialize(renderDevice, context)) {
            logger::error("Failed to initialize CEF overlay texture.");
            return false;
        }

        _impl->initialized.store(true, std::memory_order_release);
        _impl->width = width;
        _impl->height = height;
        _impl->hwnd = hwnd;
        logger::info("CefInitialize succeeded.");

        _impl->client = new CefOsrClient(width, height);

        logger::info("Scheduling CEF OSR browser creation at {}x{}.", width, height);
        PostToCefUi([this, shellUrl] {
            CefWindowInfo windowInfo;
            windowInfo.SetAsWindowless(_impl->hwnd);
            windowInfo.shared_texture_enabled = true;
            windowInfo.external_begin_frame_enabled = true;

            CefBrowserSettings browserSettings;
            browserSettings.windowless_frame_rate = Constants::TargetFps;
            browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

            CefString url;
            url.FromWString(shellUrl);
            logger::info("Requesting CEF OSR browser creation for shell URL.");
            const bool requested =
                CefBrowserHost::CreateBrowser(windowInfo, _impl->client, url, browserSettings, nullptr, nullptr);
            if (!requested) {
                logger::error("CefBrowserHost::CreateBrowser returned false.");
                _impl->initState.SetFailed();
            }
        });

        auto isSuccess = _impl->initState.WaitForInitialize();
        if (isSuccess) {
            logger::info("Initialized");
        }

        return isSuccess;
    }

    void CefRuntime::Resize(uint32_t width, uint32_t height) const {
        if (!_impl->initialized.load(std::memory_order_acquire) || width == 0 || height == 0) {
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            if (_impl->width == width && _impl->height == height) {
                return;
            }
            _impl->width = width;
            _impl->height = height;
            client = _impl->client;
        }

        if (!client) {
            return;
        }

        PostToCefUi([client, width, height]() { client->SetSize(width, height); });
    }

    void CefRuntime::BeginFrame() const {
        if (!_impl->initialized.load(std::memory_order_acquire)) {
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
        }

        if (!client || !client->HasBrowser()) {
            return;
        }

        PostToCefUi([client] { client->SendExternalBeginFrame(); });
    }

    void CefRuntime::UpdateOverlayTexture() const {
        if (_impl->initialized.load(std::memory_order_acquire)) {
            _impl->overlay.CopyPendingAcceleratedFrame();
        }
    }

    std::optional<OverlayTextureInfo> CefRuntime::GetOverlayInfo() const { return _impl->overlay.GetInfo(); }

    void CefRuntime::SubmitAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle) const {
        if (!_impl->initialized.load(std::memory_order_acquire) ||
            _impl->shuttingDown.load(std::memory_order_acquire)) {
            return;
        }

        _impl->overlay.SubmitAcceleratedFrameDuringCallback(sharedTextureHandle);
    }

    void CefRuntime::AppendShellArg(std::string& out, uint64_t value) {
        // NanoID generates uint64 across the full range; encode as a JS string
        // literal so values above 2^53-1 survive (JS shell `normalizeId` accepts
        // string-of-digits in addition to safe integers).
        out += JsLiteralString(std::to_string(value));
    }

    void CefRuntime::AppendShellArg(std::string& out, int value) { out += std::to_string(value); }

    void CefRuntime::AppendShellArg(std::string& out, bool value) { out += value ? "true" : "false"; }

    void CefRuntime::AppendShellArg(std::string& out, std::string_view value) { out += JsLiteralString(value); }

    void CefRuntime::AppendShellArg(std::string& out, const ShellCreateViewArg& value) {
        out += "{ id: ";
        AppendShellArg(out, value.id);
        out += ", url: ";
        AppendShellArg(out, value.url);
        out += ", order: ";
        AppendShellArg(out, value.order);
        out += ", hidden: ";
        AppendShellArg(out, value.hidden);
        out += " }";
    }

    bool CefRuntime::RunShellScript(std::string_view method, uint64_t viewId, std::string script) const {
        if (!_impl->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF shell '{}' (view={}) ignored: CEF is not initialized.", method, viewId);
            return false;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF shell '{}' (view={}) ignored: no browser is available.", method, viewId);
            return false;
        }

        {
            std::lock_guard lock(_impl->shellMutex);
            if (!_impl->shellReady) {
                logger::warn("CEF shell '{}' (view={}) deferred: shell is not ready.", method, viewId);
                return false;
            }
        }

        const bool checkIframe = method != std::string_view{"createView"};

        PostToCefUi([client, script = std::move(script), method = std::string(method), viewId, checkIframe]() {
            CefRefPtr<CefBrowser> browser = client->GetBrowserOnUiThread();
            if (!browser) {
                logger::error("CEF shell '{}' (view={}) failed: browser disappeared.", method, viewId);
                return;
            }

            if (checkIframe) {
                const std::string iframeName = MakeIframeName(viewId);
                CefString frameName;
                frameName.FromString(iframeName);
                if (!client->GetFrameByNameOnUiThread(frameName)) {
                    logger::warn("CEF shell '{}' did not find iframe '{}'.", method, iframeName);
                }
            }

            CefRefPtr<CefFrame> mainFrame = browser->GetMainFrame();
            if (!mainFrame) {
                logger::error("CEF shell '{}' (view={}) failed: main frame is unavailable.", method, viewId);
                return;
            }

            CefString sourceUrl;
            sourceUrl.FromASCII("prismaui://shell-command");
            CefString cefScript;
            cefScript.FromString(script);
            mainFrame->ExecuteJavaScript(cefScript, sourceUrl, 0);
            logger::debug("CEF shell '{}' (view={}) executed.", method, viewId);
        });

        return true;
    }

    void CefRuntime::ReplayShellViews() {
        std::vector<Impl::ShellViewState> views;
        {
            std::lock_guard lock(_impl->shellMutex);
            views.reserve(_impl->shellViews.size());
            for (const auto& state : _impl->shellViews | std::views::values) {
                views.push_back(state);
            }
        }

        for (const auto& state : views) {
            InvokeShell("createView", state.viewId,
                        ShellCreateViewArg{state.viewId, state.resolvedUrl, state.order, state.hidden});
            if (state.focused) {
                InvokeShell("focusView", state.viewId, state.viewId);
            }
        }
    }

    bool CefRuntime::CreateShellView(uint64_t viewId, std::string_view urlOrPath, int order, bool hidden) {
        const std::string iframeName = MakeIframeName(viewId);
        const std::string resolvedUrl = ResolveViewUrl(urlOrPath);
        {
            std::lock_guard lock(_impl->shellMutex);
            auto& state = _impl->shellViews[viewId];
            state.viewId = viewId;
            state.iframeName = iframeName;
            state.resolvedUrl = resolvedUrl;
            state.hidden = hidden;
            state.order = order;
            if (state.loadState.empty()) {
                state.loadState = "not-created";
            }
        }

        logger::info("CEF shell create view: id={}, iframe='{}', url='{}', order={}, hidden={}.", viewId, iframeName,
                     resolvedUrl, order, hidden);
        return InvokeShell("createView", viewId, ShellCreateViewArg{viewId, resolvedUrl, order, hidden}) ||
               (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::DestroyShellView(uint64_t viewId) {
        const std::string iframeName = MakeIframeName(viewId);
        bool existed = false;
        {
            std::lock_guard lock(_impl->shellMutex);
            existed = _impl->shellViews.erase(viewId) != 0;
        }

        logger::info("CEF shell destroy view: id={}, iframe='{}', existed={}.", viewId, iframeName, existed);
        if (!existed) {
            return true;
        }
        return InvokeShell("destroyView", viewId, viewId) || (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::SetShellViewHidden(uint64_t viewId, bool hidden) {
        const std::string iframeName = MakeIframeName(viewId);
        {
            std::lock_guard lock(_impl->shellMutex);
            auto it = _impl->shellViews.find(viewId);
            if (it == _impl->shellViews.end()) {
                logger::warn("CEF shell set hidden ignored missing view: id={}, iframe='{}'.", viewId, iframeName);
                return false;
            }
            it->second.hidden = hidden;
        }

        logger::info("CEF shell set hidden: id={}, iframe='{}', hidden={}.", viewId, iframeName, hidden);
        return InvokeShell("setHidden", viewId, viewId, hidden) || (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::SetShellViewOrder(uint64_t viewId, int order) {
        const std::string iframeName = MakeIframeName(viewId);
        {
            std::lock_guard lock(_impl->shellMutex);
            auto it = _impl->shellViews.find(viewId);
            if (it == _impl->shellViews.end()) {
                logger::warn("CEF shell set order ignored missing view: id={}, iframe='{}'.", viewId, iframeName);
                return false;
            }
            it->second.order = order;
        }

        logger::info("CEF shell set order: id={}, iframe='{}', order={}.", viewId, iframeName, order);
        return InvokeShell("setOrder", viewId, viewId, order) || (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::FocusShellView(uint64_t viewId) {
        const std::string iframeName = MakeIframeName(viewId);
        {
            std::lock_guard lock(_impl->shellMutex);
            auto it = _impl->shellViews.find(viewId);
            if (it == _impl->shellViews.end()) {
                logger::warn("CEF shell focus ignored missing view: id={}, iframe='{}'.", viewId, iframeName);
                return false;
            }
            for (auto& [otherId, state] : _impl->shellViews) {
                state.focused = otherId == viewId;
            }
        }

        logger::info("CEF shell focus view: id={}, iframe='{}'.", viewId, iframeName);
        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
        }
        if (client && client->HasBrowser()) {
            PostToCefUi([client, viewId]() {
                CefRefPtr<CefBrowser> browser = client->GetBrowserOnUiThread();
                CefRefPtr<CefBrowserHost> host = browser ? browser->GetHost() : nullptr;
                if (host) {
                    host->SetFocus(true);
                } else {
                    logger::warn("CEF shell focus could not focus browser host for View [{}].", viewId);
                }
            });
        }
        return InvokeShell("focusView", viewId, viewId) || (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::BlurShellView(uint64_t viewId) {
        const std::string iframeName = MakeIframeName(viewId);
        {
            std::lock_guard lock(_impl->shellMutex);
            auto it = _impl->shellViews.find(viewId);
            if (it == _impl->shellViews.end()) {
                logger::warn("CEF shell blur ignored missing view: id={}, iframe='{}'.", viewId, iframeName);
                return false;
            }
            it->second.focused = false;
        }

        logger::info("CEF shell blur view: id={}, iframe='{}'.", viewId, iframeName);
        return InvokeShell("blurView", viewId, viewId) || (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::TryGetShellFrameName(uint64_t viewId, std::string& outName) const {
        std::lock_guard lock(_impl->shellMutex);
        const auto it = _impl->shellViews.find(viewId);
        if (it == _impl->shellViews.end()) {
            outName.clear();
            return false;
        }
        outName = it->second.iframeName;
        return true;
    }

    bool CefRuntime::IsShellReady() const {
        std::lock_guard lock(_impl->shellMutex);
        return _impl->shellReady;
    }

    void CefRuntime::OpenDevTools() const {
        logger::info("CEF DevTools open requested.");

        if (!_impl->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF DevTools open ignored because CEF is not initialized.");
            return;
        }

        CefRefPtr<CefOsrClient> client;
        HWND hwnd = nullptr;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
            hwnd = _impl->hwnd;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF DevTools open ignored because no shell browser is available.");
            return;
        }

        PostToCefUi([this, client, hwnd]() {
            CefRefPtr<CefBrowser> browser = client->GetBrowserOnUiThread();
            CefRefPtr<CefBrowserHost> host = browser ? browser->GetHost() : nullptr;
            if (!browser || !host) {
                logger::error("CEF DevTools open failed: shell browser host is unavailable.");
                return;
            }

            const int targetBrowserId = browser->GetIdentifier();
            logger::info("CEF DevTools opening for shell browser [{}]; remote debugging is disabled.", targetBrowserId);

            if (host->HasDevTools()) {
                _impl->devToolsOpen.store(true, std::memory_order_release);
                _impl->devToolsTargetBrowserId.store(targetBrowserId, std::memory_order_release);
                logger::info("CEF DevTools already open for shell browser [{}]; focusing existing DevTools.",
                             targetBrowserId);
                CefWindowInfo ignoredWindowInfo;
                CefBrowserSettings ignoredSettings;
                host->ShowDevTools(ignoredWindowInfo, nullptr, ignoredSettings, CefPoint());
                return;
            }

            CefRefPtr<DevToolsClient> devToolsClient = new DevToolsClient(
                [this, targetBrowserId](CefRefPtr<CefBrowser> devToolsBrowser) {
                    if (!devToolsBrowser) {
                        _impl->devToolsOpen.store(false, std::memory_order_release);
                        _impl->devToolsBrowserId.store(-1, std::memory_order_release);
                        logger::error("CEF DevTools failed to report a browser for shell browser [{}].",
                                      targetBrowserId);
                        return;
                    }

                    {
                        std::lock_guard lock(_impl->devToolsMutex);
                        _impl->devToolsBrowser = devToolsBrowser;
                    }
                    _impl->devToolsOpen.store(true, std::memory_order_release);
                    _impl->devToolsTargetBrowserId.store(targetBrowserId, std::memory_order_release);
                    _impl->devToolsBrowserId.store(devToolsBrowser->GetIdentifier(), std::memory_order_release);
                    logger::info("CEF DevTools browser [{}] opened for shell browser [{}].",
                                 devToolsBrowser->GetIdentifier(), targetBrowserId);
                },
                [this, targetBrowserId](CefRefPtr<CefBrowser> devToolsBrowser) {
                    const int devToolsBrowserId = devToolsBrowser ? devToolsBrowser->GetIdentifier() : -1;
                    {
                        std::lock_guard lock(_impl->devToolsMutex);
                        _impl->devToolsBrowser = nullptr;
                        _impl->devToolsClient = nullptr;
                    }
                    _impl->devToolsOpen.store(false, std::memory_order_release);
                    _impl->devToolsTargetBrowserId.store(-1, std::memory_order_release);
                    _impl->devToolsBrowserId.store(-1, std::memory_order_release);
                    logger::info("CEF DevTools browser [{}] closed for shell browser [{}].", devToolsBrowserId,
                                 targetBrowserId);
                });

            {
                std::lock_guard lock(_impl->devToolsMutex);
                _impl->devToolsClient = devToolsClient;
            }

            CefWindowInfo windowInfo;
            CefString windowTitle;
            windowTitle.FromASCII("PrismaUI DevTools");
            windowInfo.SetAsPopup(hwnd, windowTitle);

            CefBrowserSettings browserSettings;
            host->ShowDevTools(windowInfo, devToolsClient, browserSettings, CefPoint());
            logger::info("CEF DevTools ShowDevTools submitted for shell browser [{}].", targetBrowserId);
        });
    }

    void CefRuntime::CloseDevTools() const {
        logger::info("CEF DevTools close requested.");

        if (!_impl->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF DevTools close ignored because CEF is not initialized.");
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF DevTools close found no shell browser.");
            {
                std::lock_guard lock(_impl->devToolsMutex);
                _impl->devToolsBrowser = nullptr;
                _impl->devToolsClient = nullptr;
            }
            _impl->devToolsOpen.store(false, std::memory_order_release);
            _impl->devToolsTargetBrowserId.store(-1, std::memory_order_release);
            _impl->devToolsBrowserId.store(-1, std::memory_order_release);
            return;
        }

        PostToCefUi([this, client]() {
            CefRefPtr<CefBrowser> browser = client->GetBrowserOnUiThread();
            CefRefPtr<CefBrowserHost> host = browser ? browser->GetHost() : nullptr;
            if (!browser || !host) {
                logger::error("CEF DevTools close failed: shell browser host is unavailable.");
                return;
            }

            const int targetBrowserId = browser->GetIdentifier();
            if (!host->HasDevTools()) {
                logger::info("CEF DevTools close no-op: shell browser [{}] has no DevTools.", targetBrowserId);
                {
                    std::lock_guard lock(_impl->devToolsMutex);
                    _impl->devToolsBrowser = nullptr;
                    _impl->devToolsClient = nullptr;
                }
                _impl->devToolsOpen.store(false, std::memory_order_release);
                _impl->devToolsTargetBrowserId.store(-1, std::memory_order_release);
                _impl->devToolsBrowserId.store(-1, std::memory_order_release);
                return;
            }

            logger::info("CEF DevTools CloseDevTools submitted for shell browser [{}], tracked DevTools browser [{}].",
                         targetBrowserId, _impl->devToolsBrowserId.load(std::memory_order_acquire));
            host->CloseDevTools();
        });
    }

    bool CefRuntime::IsDevToolsOpen() const { return _impl->devToolsOpen.load(std::memory_order_acquire); }

    void CefRuntime::NotifyShellLoadStart(const std::string& frameIdentifier, const std::string& url) const {
        std::lock_guard lock(_impl->shellMutex);
        _impl->shellReady = false;
        _impl->shellFrameIdentifier = frameIdentifier;
        _impl->shellUrl = url;
        logger::info("CEF shell state: load start, frame id '{}', url '{}'.", frameIdentifier, url);
    }

    void CefRuntime::NotifyShellLoadEnd(int httpStatusCode, const std::string& frameIdentifier,
                                        const std::string& url) {
        {
            std::lock_guard lock(_impl->shellMutex);
            _impl->shellReady = true;
            _impl->shellFrameIdentifier = frameIdentifier;
            _impl->shellUrl = url;
        }
        logger::info("CEF shell state: ready, frame id '{}', status {}, url '{}'.", frameIdentifier, httpStatusCode,
                     url);
        ReplayShellViews();
        _impl->initState.SetInitialized();
    }

    void CefRuntime::NotifyShellLoadError(int errorCode, const std::string& errorText, const std::string& failedUrl,
                                          const std::string& frameIdentifier, const std::string& url) const {
        std::lock_guard lock(_impl->shellMutex);
        _impl->shellReady = false;
        _impl->shellFrameIdentifier = frameIdentifier;
        _impl->shellUrl = url;
        logger::error("CEF shell state: load error code={}, error='{}', failedUrl='{}', frame id '{}', url '{}'.",
                      errorCode, errorText, failedUrl, frameIdentifier, url);
        _impl->initState.SetFailed();
    }

    void CefRuntime::NotifyShellFrameLoadStart(const std::string& frameName, const std::string& frameIdentifier,
                                               const std::string& url) const {
        uint64_t viewId = 0;
        if (!ViewUtils::TryParseViewIdFromFrameName(frameName, viewId)) {
            logger::debug("CEF shell ignored nested iframe load start: frame='{}', id='{}', url='{}'.", frameName,
                          frameIdentifier, url);
            return;
        }

        std::lock_guard lock(_impl->shellMutex);
        auto it = _impl->shellViews.find(viewId);
        if (it == _impl->shellViews.end()) {
            logger::warn("CEF shell iframe load start for unknown view: id={}, iframe='{}', frame id '{}', url='{}'.",
                         viewId, frameName, frameIdentifier, url);
            return;
        }
        it->second.loadState = "loading";
        it->second.lastFrameIdentifier = frameIdentifier;
        it->second.lastFrameName = frameName;
        logger::info("CEF shell iframe state: id={}, iframe='{}', loading url='{}'.", viewId, frameName, url);
    }

    void CefRuntime::NotifyShellFrameLoadEnd(const std::string& frameName, const std::string& frameIdentifier,
                                             const std::string& url, int httpStatusCode) const {
        uint64_t viewId = 0;
        if (!ViewUtils::TryParseViewIdFromFrameName(frameName, viewId)) {
            logger::debug("CEF shell ignored nested iframe load end: frame='{}', id='{}', status {}, url='{}'.",
                          frameName, frameIdentifier, httpStatusCode, url);
            return;
        }

        std::lock_guard lock(_impl->shellMutex);
        auto it = _impl->shellViews.find(viewId);
        if (it == _impl->shellViews.end()) {
            logger::warn(
                "CEF shell iframe load end for unknown view: id={}, iframe='{}', frame id '{}', status {}, url='{}'.",
                viewId, frameName, frameIdentifier, httpStatusCode, url);
            return;
        }
        it->second.loadState = "loaded";
        it->second.lastFrameIdentifier = frameIdentifier;
        it->second.lastFrameName = frameName;
        logger::info("CEF shell iframe state: id={}, iframe='{}', loaded status {}, url='{}'.", viewId, frameName,
                     httpStatusCode, url);
    }

    void CefRuntime::NotifyShellFrameLoadError(const std::string& frameName, const std::string& frameIdentifier,
                                               const std::string& url, int errorCode, const std::string& errorText,
                                               const std::string& failedUrl) const {
        uint64_t viewId = 0;
        if (!ViewUtils::TryParseViewIdFromFrameName(frameName, viewId)) {
            logger::debug("CEF shell ignored nested iframe load error: frame='{}', id='{}', code={}, failedUrl='{}'.",
                          frameName, frameIdentifier, errorCode, failedUrl);
            return;
        }

        std::lock_guard lock(_impl->shellMutex);
        auto it = _impl->shellViews.find(viewId);
        if (it == _impl->shellViews.end()) {
            logger::warn(
                "CEF shell iframe load error for unknown view: id={}, iframe='{}', frame id '{}', code={}, "
                "failedUrl='{}'.",
                viewId, frameName, frameIdentifier, errorCode, failedUrl);
            return;
        }
        it->second.loadState = "error";
        it->second.lastFrameIdentifier = frameIdentifier;
        it->second.lastFrameName = frameName;
        logger::error(
            "CEF shell iframe state: id={}, iframe='{}', load error code={}, error='{}', failedUrl='{}', url='{}'.",
            viewId, frameName, errorCode, errorText, failedUrl, url);
    }

    void CefRuntime::Shutdown() const {
        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            if (!_impl->initialized.load(std::memory_order_acquire)) {
                return;
            }

            if (_impl->shuttingDown.exchange(true, std::memory_order_acq_rel)) {
                logger::warn("CEF shutdown is already in progress.");
                return;
            }

            client = _impl->client;
        }

        _impl->overlay.ReleaseResources();

        logger::info("CEF runtime shutdown started.");

        bool browserClosed = true;
        if (client && client->HasBrowser()) {
            client->ResetCloseSignal();
            PostToCefUi([this, client]() {
                CefRefPtr<CefBrowser> browser = client->GetBrowserOnUiThread();
                CefRefPtr<CefBrowserHost> host = browser ? browser->GetHost() : nullptr;
                if (browser && host && host->HasDevTools()) {
                    logger::info("CEF shutdown closing DevTools for shell browser [{}], tracked DevTools browser [{}].",
                                 browser->GetIdentifier(), _impl->devToolsBrowserId.load(std::memory_order_acquire));
                    host->CloseDevTools();
                }
                client->CloseBrowser();
            });
            browserClosed = client->WaitForClose(kBrowserCloseTimeout);
            if (browserClosed) {
                logger::info("CEF browser closed before shutdown.");
            } else {
                logger::error(
                    "Timed out waiting for CEF browser close; skipping CefShutdown to avoid a shutdown deadlock.");
            }
        } else {
            logger::info("CEF shutdown found no live browser.");
        }

        if (!browserClosed) {
            _impl->shutdownSkipped.store(true, std::memory_order_release);
            _impl->shuttingDown.store(false, std::memory_order_release);
            return;
        }

        {
            std::lock_guard lock(_impl->shellMutex);
            if (!_impl->shellViews.empty()) {
                logger::info("Clearing {} CEF shell view states during shutdown.", _impl->shellViews.size());
            }
            _impl->shellViews.clear();
            _impl->shellReady = false;
            _impl->shellFrameIdentifier.clear();
            _impl->shellUrl.clear();
        }

        {
            std::lock_guard lock(_impl->devToolsMutex);
            _impl->devToolsBrowser = nullptr;
            _impl->devToolsClient = nullptr;
        }
        _impl->devToolsOpen.store(false, std::memory_order_release);
        _impl->devToolsTargetBrowserId.store(-1, std::memory_order_release);
        _impl->devToolsBrowserId.store(-1, std::memory_order_release);

        logger::info("Calling CefShutdown.");
        CefShutdown();
        logger::info("CefShutdown completed.");

        {
            std::lock_guard lock(_impl->stateMutex);
            _impl->client = nullptr;
            _impl->app = nullptr;
            _impl->width = 0;
            _impl->height = 0;
            _impl->hwnd = nullptr;
            _impl->initialized.store(false, std::memory_order_release);
            _impl->shuttingDown.store(false, std::memory_order_release);
        }
    }

    bool CefRuntime::IsInitialized() const { return _impl->initialized.load(std::memory_order_acquire); }

    bool CefRuntime::HasBrowser() const {
        std::lock_guard lock(_impl->stateMutex);
        return _impl->client && _impl->client->HasBrowser();
    }

    namespace {
        cef_key_event_type_t ToCefKeyType(CefInputKeyType type) {
            switch (type) {
                case CefInputKeyType::RawKeyDown:
                    return KEYEVENT_RAWKEYDOWN;
                case CefInputKeyType::KeyUp:
                    return KEYEVENT_KEYUP;
                case CefInputKeyType::Char:
                    return KEYEVENT_CHAR;
                default:
                    return KEYEVENT_RAWKEYDOWN;
            }
        }

        cef_mouse_button_type_t ToCefMouseButton(CefInputMouseButton button) {
            switch (button) {
                case CefInputMouseButton::Left:
                    return MBT_LEFT;
                case CefInputMouseButton::Middle:
                    return MBT_MIDDLE;
                case CefInputMouseButton::Right:
                    return MBT_RIGHT;
                default:
                    return MBT_LEFT;
            }
        }
    }

    void CefRuntime::DispatchInputEvents(uint64_t viewId, std::vector<CefInputEvent> events) const {
        if (events.empty()) {
            return;
        }

        if (!_impl->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: runtime unavailable.", events.size(),
                         viewId);
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: browser unavailable.", events.size(),
                         viewId);
            return;
        }

        PostToCefUi([this, client, viewId, events = std::move(events)]() mutable {
            std::string iframeName;
            {
                std::lock_guard lock(_impl->shellMutex);
                const auto it = _impl->shellViews.find(viewId);
                if (it == _impl->shellViews.end()) {
                    logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: missing shell view.",
                                 events.size(), viewId);
                    return;
                }
                if (it->second.hidden) {
                    logger::debug("CEF input dispatch dropped {} event(s) for hidden View [{}].", events.size(),
                                  viewId);
                    return;
                }
                if (!_impl->shellReady) {
                    logger::debug("CEF input dispatch dropped {} event(s) for View [{}]: shell not ready.",
                                  events.size(), viewId);
                    return;
                }
                iframeName = it->second.iframeName;
            }

            CefRefPtr<CefBrowser> browser = client->GetBrowserOnUiThread();
            if (!browser) {
                logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: browser disappeared.",
                             events.size(), viewId);
                return;
            }

            CefString frameName;
            frameName.FromString(iframeName);
            if (!client->GetFrameByNameOnUiThread(frameName)) {
                logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: iframe '{}' is missing.",
                             events.size(), viewId, iframeName);
                return;
            }

            CefRefPtr<CefBrowserHost> host = browser->GetHost();
            if (!host) {
                logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: browser host unavailable.",
                             events.size(), viewId);
                return;
            }

            host->SetFocus(true);

            for (const auto& event : events) {
                std::visit(
                    [host, viewId]<typename T0>(const T0& value) {
                        using T = std::decay_t<T0>;
                        if constexpr (std::is_same_v<T, CefInputMouseMove>) {
                            CefMouseEvent mouseEvent;
                            mouseEvent.x = value.x;
                            mouseEvent.y = value.y;
                            mouseEvent.modifiers = value.modifiers;
                            host->SendMouseMoveEvent(mouseEvent, value.mouseLeave);
                        } else if constexpr (std::is_same_v<T, CefInputMouseClick>) {
                            CefMouseEvent mouseEvent;
                            mouseEvent.x = value.x;
                            mouseEvent.y = value.y;
                            mouseEvent.modifiers = value.modifiers;
                            host->SendMouseClickEvent(mouseEvent, ToCefMouseButton(value.button), value.mouseUp,
                                                      value.clickCount);
                        } else if constexpr (std::is_same_v<T, CefInputMouseWheel>) {
                            CefMouseEvent mouseEvent;
                            mouseEvent.x = value.x;
                            mouseEvent.y = value.y;
                            mouseEvent.modifiers = value.modifiers;
                            host->SendMouseWheelEvent(mouseEvent, value.deltaX, value.deltaY);
                        } else if constexpr (std::is_same_v<T, CefInputKey>) {
                            CefKeyEvent keyEvent;
                            keyEvent.type = ToCefKeyType(value.type);
                            keyEvent.modifiers = value.modifiers;
                            keyEvent.windows_key_code = value.windowsKeyCode;
                            keyEvent.native_key_code = value.nativeKeyCode;
                            keyEvent.character = value.character;
                            keyEvent.unmodified_character = value.unmodifiedCharacter;
                            keyEvent.is_system_key = value.isSystemKey ? 1 : 0;
                            keyEvent.focus_on_editable_field = value.focusOnEditableField ? 1 : 0;
                            host->SendKeyEvent(keyEvent);
                        } else {
                            logger::warn("CEF input dispatch for View [{}] encountered unsupported event.", viewId);
                        }
                    },
                    event);
            }
        });
    }

    namespace {
        // Find the iframe frame on the CEF UI thread. Returns nullptr if the iframe
        // does not (yet) exist — caller is responsible for logging.
        CefRefPtr<CefFrame> FindIframeFrame(CefRefPtr<CefOsrClient> client, uint64_t viewId) {
            if (!client) return nullptr;
            const std::string iframeName = MakeIframeName(viewId);
            CefString frameName;
            frameName.FromString(iframeName);
            return client->GetFrameByNameOnUiThread(frameName);
        }
    }

    void CefRuntime::InvokeScript(uint64_t viewId, std::string script,
                                  std::function<void(std::string)> callback) const {
        if (!_impl->initialized.load(std::memory_order_acquire)) {
            logger::warn("InvokeScript: CEF not initialized; firing empty callback for view [{}].", viewId);
            if (callback) callback(std::string());
            return;
        }

        const uint64_t requestId = _impl->nextRequestId.fetch_add(1, std::memory_order_relaxed);

        if (callback) {
            std::lock_guard lock(_impl->invokeMutex);
            Impl::InvokeEntry entry;
            entry.viewId = viewId;
            entry.callback = std::move(callback);
            _impl->pendingInvokes.emplace(requestId, std::move(entry));
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
        }

        const std::string scriptCopy = std::move(script);

        PostToCefUi([this, client, viewId, requestId, scriptCopy]() {
            CefRefPtr<CefFrame> frame = FindIframeFrame(client, viewId);
            if (!frame) {
                logger::warn("InvokeScript: iframe for view [{}] not yet attached; failing request {}.", viewId,
                             requestId);
                // Fire the queued callback with an empty string and remove the entry.
                Impl::InvokeEntry drained;
                bool have = false;
                {
                    std::lock_guard lock(_impl->invokeMutex);
                    auto it = _impl->pendingInvokes.find(requestId);
                    if (it != _impl->pendingInvokes.end()) {
                        drained = std::move(it->second);
                        _impl->pendingInvokes.erase(it);
                        have = true;
                    }
                }
                if (have && drained.callback) drained.callback(std::string());
                return;
            }

            Messaging::SendProcessMessageToFrame(
                frame, PID_RENDERER, BTRMessages::InvokeRequestMessage{.RequestId = requestId, .Script = scriptCopy});
        });
    }

    void CefRuntime::InteropCallInView(uint64_t viewId, std::string functionName, std::string argument) const {
        if (!_impl->initialized.load(std::memory_order_acquire)) {
            logger::warn("InteropCall: CEF not initialized; ignoring call to '{}' on view [{}].", functionName, viewId);
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
        }

        PostToCefUi([client, viewId, fn = std::move(functionName), arg = std::move(argument)]() {
            CefRefPtr<CefFrame> frame = FindIframeFrame(client, viewId);
            if (!frame) {
                logger::warn("InteropCall: iframe for view [{}] is not attached; dropping call to '{}'.", viewId, fn);
                return;
            }

            Messaging::SendProcessMessageToFrame(frame, PID_RENDERER,
                                                 BTRMessages::InteropCallMessage{.FunctionName = fn, .Argument = arg});
        });
    }

    void CefRuntime::RegisterListener(uint64_t viewId, std::string name,
                                      std::function<void(const std::string&)> /*callback*/) const {
        // The callback itself lives in Core::jsCallbacks; this method only forwards
        // the "install trampoline" message to the renderer so the iframe exposes a
        // window[name] = function(arg) bridge. Caller (Communication::RegisterJSListener)
        // is responsible for storing the callback first.
        if (!_impl->initialized.load(std::memory_order_acquire)) {
            logger::warn("RegisterListener: CEF not initialized; '{}' for view [{}] will be installed lazily.", name,
                         viewId);
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(_impl->stateMutex);
            client = _impl->client;
        }

        const std::string nameCopy = std::move(name);

        PostToCefUi([client, viewId, nameCopy]() {
            CefRefPtr<CefFrame> frame = FindIframeFrame(client, viewId);
            if (!frame) {
                // No frame yet — renderer will queue installs at OnContextCreated once
                // the iframe lands. We still try once here; if the frame appears later
                // the listener is re-registered when the iframe re-creates its context.
                logger::info(
                    "RegisterListener: iframe for view [{}] not yet attached; '{}' will install at next context.",
                    viewId, nameCopy);
                return;
            }
            logger::info("RegisterListener: installing '{}' for view [{}].", nameCopy, viewId);
            Messaging::SendProcessMessageToFrame(frame, PID_RENDERER,
                                                 BTRMessages::InstallListenerMessage{.ListenerName = nameCopy});
        });
    }

    void CefRuntime::CancelInvokesForView(uint64_t viewId) const {
        std::vector<Impl::InvokeEntry> drained;
        {
            std::lock_guard lock(_impl->invokeMutex);
            for (auto it = _impl->pendingInvokes.begin(); it != _impl->pendingInvokes.end();) {
                if (it->second.viewId == viewId) {
                    drained.push_back(std::move(it->second));
                    it = _impl->pendingInvokes.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!drained.empty()) {
            logger::info("CancelInvokesForView: draining {} pending Invoke callback(s) for view [{}].", drained.size(),
                         viewId);
        }
        for (auto& entry : drained) {
            if (entry.callback) entry.callback(std::string());
        }
    }

    bool CefRuntime::OnRendererMessage(const CefString& frameName,
                                       const RTBMessages::RendererToBrowserMessage& message) const {
        std::uint64_t frameViewId = 0;
        const bool hasFrameViewId = ViewUtils::TryParseViewIdFromFrameName(frameName, frameViewId);

        auto requireFrameView = [&frameName, hasFrameViewId](const char* messageName) {
            if (!hasFrameViewId) {
                logger::warn("OnRendererMessage: {} arrived from non-view frame '{}' - refusing.", messageName,
                             frameName.ToString());
                return false;
            }

            return true;
        };

        Match(
            message,
            [this](const RTBMessages::InvokeResultMessage& m) {
                Impl::InvokeEntry entry;
                {
                    std::lock_guard lock(_impl->invokeMutex);
                    auto it = _impl->pendingInvokes.find(m.RequestId);
                    if (it != _impl->pendingInvokes.end()) {
                        entry = std::move(it->second);
                        _impl->pendingInvokes.erase(it);
                    }
                }
                if (entry.callback) {
                    entry.callback(m.Success ? m.Result.ToString() : std::string());
                }
            },
            [&requireFrameView, frameViewId](const RTBMessages::ListenerInvokeMessage& m) {
                if (requireFrameView("listenerInvoke")) {
                    Communication::DispatchListenerInvoke(frameViewId, m.ListenerName.ToString(),
                                                          m.Argument.ToString());
                }
            },
            [&requireFrameView, frameViewId](const RTBMessages::ConsoleMessage& m) {
                if (requireFrameView("consoleMessage")) {
                    Communication::DispatchConsoleMessage(frameViewId, m.Level.ToString(), m.Text.ToString());
                }
            },
            [&requireFrameView, frameViewId](const RTBMessages::DomReadyMessage&) {
                if (requireFrameView("domReady")) {
                    Communication::DispatchDomReady(frameViewId);
                }
            });

        return true;
    }
}
