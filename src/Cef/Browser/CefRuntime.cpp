#include "PCH.h"

#ifdef GetNextSibling
    #undef GetNextSibling
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

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
#include "include/cef_process_message.h"
#include "include/cef_task.h"
#include "include/cef_values.h"
#include "include/wrapper/cef_helpers.h"

namespace {
    constexpr int kCefWindowlessFrameRate = 120;
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

    class FunctionTask final : public CefTask {
    public:
        explicit FunctionTask(std::function<void()> task) : task_(std::move(task)) {}

        void Execute() override {
            try {
                if (task_) {
                    task_();
                }
            } catch (const std::exception& e) {
                logger::error("Exception in CEF UI task: {}", e.what());
            } catch (...) {
                logger::error("Unknown exception in CEF UI task.");
            }
        }

    private:
        std::function<void()> task_;

        IMPLEMENT_REFCOUNTING(FunctionTask);
    };

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
        bool missingD3DLogged = false;

        // ---- Step 7 JS bridge state ----
        struct InvokeEntry {
            uint64_t viewId = 0;
            std::function<void(std::string)> callback;
        };
        std::mutex invokeMutex;
        std::atomic<uint64_t> nextRequestId = 1;
        std::map<uint64_t, InvokeEntry> pendingInvokes;
    };

    CefRuntime::CefRuntime() : impl_(std::make_unique<Impl>()) {}

    CefRuntime::~CefRuntime() = default;

    CefRuntime& CefRuntime::GetSingleton() {
        static CefRuntime instance;
        return instance;
    }

    bool CefRuntime::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
        std::lock_guard lock(impl_->stateMutex);

        if (impl_->initialized.load(std::memory_order_acquire)) {
            return true;
        }

        if (!hwnd || width == 0 || height == 0) {
            logger::debug("CEF initialization deferred: hwnd={}, size={}x{}", hwnd ? "set" : "null", width, height);
            return false;
        }

        if (impl_->initializeAttempted.exchange(true, std::memory_order_acq_rel)) {
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

        impl_->app = CreatePrismaCefApp();
        logger::info("Calling CefInitialize.");
        if (!CefInitialize(mainArgs, settings, impl_->app, nullptr)) {
            logger::error("CefInitialize failed.");
            impl_->app = nullptr;
            return false;
        }

        impl_->initialized.store(true, std::memory_order_release);
        impl_->width = width;
        impl_->height = height;
        impl_->hwnd = hwnd;
        logger::info("CefInitialize succeeded.");

        impl_->client = new CefOsrClient(width, height);
        CefRefPtr<CefOsrClient> client = impl_->client;

        logger::info("Scheduling CEF OSR browser creation at {}x{}.", width, height);
        PostToCefUi([hwnd, client, shellUrl]() {
            CefWindowInfo windowInfo;
            windowInfo.SetAsWindowless(hwnd);
            windowInfo.shared_texture_enabled = true;
            windowInfo.external_begin_frame_enabled = true;

            CefBrowserSettings browserSettings;
            browserSettings.windowless_frame_rate = kCefWindowlessFrameRate;
            browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

            CefString url;
            url.FromWString(shellUrl);
            logger::info("Requesting CEF OSR browser creation for shell URL.");
            const bool requested =
                CefBrowserHost::CreateBrowser(windowInfo, client, url, browserSettings, nullptr, nullptr);
            if (!requested) {
                logger::error("CefBrowserHost::CreateBrowser returned false.");
            }
        });

        return true;
    }

    void CefRuntime::Resize(uint32_t width, uint32_t height) {
        if (!impl_->initialized.load(std::memory_order_acquire) || width == 0 || height == 0) {
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            if (impl_->width == width && impl_->height == height) {
                return;
            }
            impl_->width = width;
            impl_->height = height;
            client = impl_->client;
        }

        if (!client) {
            return;
        }

        PostToCefUi([client, width, height]() { client->SetSize(width, height); });
    }

    void CefRuntime::BeginFrame() {
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
        }

        if (!client || !client->HasBrowser()) {
            return;
        }

        PostToCefUi([client]() { client->SendExternalBeginFrame(); });
    }

    void CefRuntime::UpdateOverlayTexture(ID3D11Device* device, ID3D11DeviceContext* context) {
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
        }

        if (!client) {
            return;
        }

        if (!device || !context) {
            if (!impl_->missingD3DLogged) {
                logger::warn("CEF overlay texture update skipped: D3D device/context is missing.");
                impl_->missingD3DLogged = true;
            }
            return;
        }
        impl_->missingD3DLogged = false;

        impl_->overlay.BindRenderDevice(device, context);

        if (impl_->overlay.RealizePendingAccelerated()) {
            PostToCefUi([client]() {
                client->InvalidateView();
                client->SendExternalBeginFrame();
            });
        }

        std::vector<std::byte> cpuFrame;
        uint32_t cpuWidth = 0;
        uint32_t cpuHeight = 0;
        uint32_t cpuStride = 0;
        if (!client->ConsumeCpuFrame(cpuFrame, cpuWidth, cpuHeight, cpuStride)) {
            return;
        }

        if (cpuFrame.empty() || cpuWidth == 0 || cpuHeight == 0 || cpuStride == 0) {
            logger::warn("CEF CPU fallback frame ignored because it was empty or dimensionless.");
            return;
        }

        if (cpuWidth > std::numeric_limits<uint32_t>::max() / 4U) {
            logger::error("CEF CPU fallback frame ignored because width {} overflows BGRA stride.", cpuWidth);
            return;
        }

        const uint32_t rowBytes = cpuWidth * 4U;
        if (cpuStride < rowBytes) {
            logger::error("CEF CPU fallback frame ignored: stride {} is smaller than row bytes {}.", cpuStride,
                          rowBytes);
            return;
        }

        impl_->overlay.UploadBgra32(cpuFrame.data(), cpuWidth, cpuHeight, cpuStride);
    }

    ID3D11ShaderResourceView* CefRuntime::GetOverlaySrv() const { return impl_->overlay.GetSrv(); }

    uint32_t CefRuntime::GetOverlayWidth() const { return impl_->overlay.GetWidth(); }

    uint32_t CefRuntime::GetOverlayHeight() const { return impl_->overlay.GetHeight(); }

    bool CefRuntime::CopyAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle) {
        return impl_->overlay.CopyFromSharedHandle(sharedTextureHandle);
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

    bool CefRuntime::RunShellScript(std::string_view method, uint64_t viewId, std::string script) {
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF shell '{}' (view={}) ignored: CEF is not initialized.", method, viewId);
            return false;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF shell '{}' (view={}) ignored: no browser is available.", method, viewId);
            return false;
        }

        {
            std::lock_guard lock(impl_->shellMutex);
            if (!impl_->shellReady) {
                logger::warn("CEF shell '{}' (view={}) deferred: shell is not ready.", method, viewId);
                return false;
            }
        }

        const bool checkIframe = method != std::string_view{"createView"};
        auto run = [client, script = std::move(script), method = std::string(method), viewId, checkIframe]() {
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
        };

        if (CefCurrentlyOn(TID_UI)) {
            run();
        } else {
            PostToCefUi(std::move(run));
        }
        return true;
    }

    void CefRuntime::ReplayShellViews() {
        std::vector<Impl::ShellViewState> views;
        {
            std::lock_guard lock(impl_->shellMutex);
            views.reserve(impl_->shellViews.size());
            for (const auto& state : impl_->shellViews | std::views::values) {
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
            std::lock_guard lock(impl_->shellMutex);
            auto& state = impl_->shellViews[viewId];
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
            std::lock_guard lock(impl_->shellMutex);
            existed = impl_->shellViews.erase(viewId) != 0;
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
            std::lock_guard lock(impl_->shellMutex);
            auto it = impl_->shellViews.find(viewId);
            if (it == impl_->shellViews.end()) {
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
            std::lock_guard lock(impl_->shellMutex);
            auto it = impl_->shellViews.find(viewId);
            if (it == impl_->shellViews.end()) {
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
            std::lock_guard lock(impl_->shellMutex);
            auto it = impl_->shellViews.find(viewId);
            if (it == impl_->shellViews.end()) {
                logger::warn("CEF shell focus ignored missing view: id={}, iframe='{}'.", viewId, iframeName);
                return false;
            }
            for (auto& [otherId, state] : impl_->shellViews) {
                state.focused = otherId == viewId;
            }
        }

        logger::info("CEF shell focus view: id={}, iframe='{}'.", viewId, iframeName);
        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
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
            std::lock_guard lock(impl_->shellMutex);
            auto it = impl_->shellViews.find(viewId);
            if (it == impl_->shellViews.end()) {
                logger::warn("CEF shell blur ignored missing view: id={}, iframe='{}'.", viewId, iframeName);
                return false;
            }
            it->second.focused = false;
        }

        logger::info("CEF shell blur view: id={}, iframe='{}'.", viewId, iframeName);
        return InvokeShell("blurView", viewId, viewId) || (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::TryGetShellFrameName(uint64_t viewId, std::string& outName) const {
        std::lock_guard lock(impl_->shellMutex);
        const auto it = impl_->shellViews.find(viewId);
        if (it == impl_->shellViews.end()) {
            outName.clear();
            return false;
        }
        outName = it->second.iframeName;
        return true;
    }

    bool CefRuntime::IsShellReady() const {
        std::lock_guard lock(impl_->shellMutex);
        return impl_->shellReady;
    }

    void CefRuntime::OpenDevTools() {
        logger::info("CEF DevTools open requested.");

        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF DevTools open ignored because CEF is not initialized.");
            return;
        }

        CefRefPtr<CefOsrClient> client;
        HWND hwnd = nullptr;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
            hwnd = impl_->hwnd;
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
                impl_->devToolsOpen.store(true, std::memory_order_release);
                impl_->devToolsTargetBrowserId.store(targetBrowserId, std::memory_order_release);
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
                        impl_->devToolsOpen.store(false, std::memory_order_release);
                        impl_->devToolsBrowserId.store(-1, std::memory_order_release);
                        logger::error("CEF DevTools failed to report a browser for shell browser [{}].",
                                      targetBrowserId);
                        return;
                    }

                    {
                        std::lock_guard lock(impl_->devToolsMutex);
                        impl_->devToolsBrowser = devToolsBrowser;
                    }
                    impl_->devToolsOpen.store(true, std::memory_order_release);
                    impl_->devToolsTargetBrowserId.store(targetBrowserId, std::memory_order_release);
                    impl_->devToolsBrowserId.store(devToolsBrowser->GetIdentifier(), std::memory_order_release);
                    logger::info("CEF DevTools browser [{}] opened for shell browser [{}].",
                                 devToolsBrowser->GetIdentifier(), targetBrowserId);
                },
                [this, targetBrowserId](CefRefPtr<CefBrowser> devToolsBrowser) {
                    const int devToolsBrowserId = devToolsBrowser ? devToolsBrowser->GetIdentifier() : -1;
                    {
                        std::lock_guard lock(impl_->devToolsMutex);
                        impl_->devToolsBrowser = nullptr;
                        impl_->devToolsClient = nullptr;
                    }
                    impl_->devToolsOpen.store(false, std::memory_order_release);
                    impl_->devToolsTargetBrowserId.store(-1, std::memory_order_release);
                    impl_->devToolsBrowserId.store(-1, std::memory_order_release);
                    logger::info("CEF DevTools browser [{}] closed for shell browser [{}].", devToolsBrowserId,
                                 targetBrowserId);
                });

            {
                std::lock_guard lock(impl_->devToolsMutex);
                impl_->devToolsClient = devToolsClient;
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

    void CefRuntime::CloseDevTools() {
        logger::info("CEF DevTools close requested.");

        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF DevTools close ignored because CEF is not initialized.");
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF DevTools close found no shell browser.");
            {
                std::lock_guard lock(impl_->devToolsMutex);
                impl_->devToolsBrowser = nullptr;
                impl_->devToolsClient = nullptr;
            }
            impl_->devToolsOpen.store(false, std::memory_order_release);
            impl_->devToolsTargetBrowserId.store(-1, std::memory_order_release);
            impl_->devToolsBrowserId.store(-1, std::memory_order_release);
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
                    std::lock_guard lock(impl_->devToolsMutex);
                    impl_->devToolsBrowser = nullptr;
                    impl_->devToolsClient = nullptr;
                }
                impl_->devToolsOpen.store(false, std::memory_order_release);
                impl_->devToolsTargetBrowserId.store(-1, std::memory_order_release);
                impl_->devToolsBrowserId.store(-1, std::memory_order_release);
                return;
            }

            logger::info("CEF DevTools CloseDevTools submitted for shell browser [{}], tracked DevTools browser [{}].",
                         targetBrowserId, impl_->devToolsBrowserId.load(std::memory_order_acquire));
            host->CloseDevTools();
        });
    }

    bool CefRuntime::IsDevToolsOpen() const { return impl_->devToolsOpen.load(std::memory_order_acquire); }

    void CefRuntime::NotifyShellLoadStart(const std::string& frameIdentifier, const std::string& url) {
        std::lock_guard lock(impl_->shellMutex);
        impl_->shellReady = false;
        impl_->shellFrameIdentifier = frameIdentifier;
        impl_->shellUrl = url;
        logger::info("CEF shell state: load start, frame id '{}', url '{}'.", frameIdentifier, url);
    }

    void CefRuntime::NotifyShellLoadEnd(int httpStatusCode, const std::string& frameIdentifier,
                                        const std::string& url) {
        {
            std::lock_guard lock(impl_->shellMutex);
            impl_->shellReady = true;
            impl_->shellFrameIdentifier = frameIdentifier;
            impl_->shellUrl = url;
        }
        logger::info("CEF shell state: ready, frame id '{}', status {}, url '{}'.", frameIdentifier, httpStatusCode,
                     url);
        ReplayShellViews();
    }

    void CefRuntime::NotifyShellLoadError(int errorCode, const std::string& errorText, const std::string& failedUrl,
                                          const std::string& frameIdentifier, const std::string& url) {
        std::lock_guard lock(impl_->shellMutex);
        impl_->shellReady = false;
        impl_->shellFrameIdentifier = frameIdentifier;
        impl_->shellUrl = url;
        logger::error("CEF shell state: load error code={}, error='{}', failedUrl='{}', frame id '{}', url '{}'.",
                      errorCode, errorText, failedUrl, frameIdentifier, url);
    }

    void CefRuntime::NotifyShellFrameLoadStart(const std::string& frameName, const std::string& frameIdentifier,
                                               const std::string& url) {
        uint64_t viewId = 0;
        if (!ViewUtils::TryParseViewIdFromFrameName(frameName, viewId)) {
            logger::debug("CEF shell ignored nested iframe load start: frame='{}', id='{}', url='{}'.", frameName,
                          frameIdentifier, url);
            return;
        }

        std::lock_guard lock(impl_->shellMutex);
        auto it = impl_->shellViews.find(viewId);
        if (it == impl_->shellViews.end()) {
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
                                             const std::string& url, int httpStatusCode) {
        uint64_t viewId = 0;
        if (!ViewUtils::TryParseViewIdFromFrameName(frameName, viewId)) {
            logger::debug("CEF shell ignored nested iframe load end: frame='{}', id='{}', status {}, url='{}'.",
                          frameName, frameIdentifier, httpStatusCode, url);
            return;
        }

        std::lock_guard lock(impl_->shellMutex);
        auto it = impl_->shellViews.find(viewId);
        if (it == impl_->shellViews.end()) {
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
                                               const std::string& failedUrl) {
        uint64_t viewId = 0;
        if (!ViewUtils::TryParseViewIdFromFrameName(frameName, viewId)) {
            logger::debug("CEF shell ignored nested iframe load error: frame='{}', id='{}', code={}, failedUrl='{}'.",
                          frameName, frameIdentifier, errorCode, failedUrl);
            return;
        }

        std::lock_guard lock(impl_->shellMutex);
        auto it = impl_->shellViews.find(viewId);
        if (it == impl_->shellViews.end()) {
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

    void CefRuntime::Shutdown() {
        impl_->overlay.ReleaseResources();
        impl_->missingD3DLogged = false;

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            if (!impl_->initialized.load(std::memory_order_acquire)) {
                return;
            }

            if (impl_->shuttingDown.exchange(true, std::memory_order_acq_rel)) {
                logger::warn("CEF shutdown is already in progress.");
                return;
            }

            client = impl_->client;
        }

        logger::info("CEF runtime shutdown started.");

        bool browserClosed = true;
        if (client && client->HasBrowser()) {
            client->ResetCloseSignal();
            PostToCefUi([this, client]() {
                CefRefPtr<CefBrowser> browser = client->GetBrowserOnUiThread();
                CefRefPtr<CefBrowserHost> host = browser ? browser->GetHost() : nullptr;
                if (browser && host && host->HasDevTools()) {
                    logger::info("CEF shutdown closing DevTools for shell browser [{}], tracked DevTools browser [{}].",
                                 browser->GetIdentifier(), impl_->devToolsBrowserId.load(std::memory_order_acquire));
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
            impl_->shutdownSkipped.store(true, std::memory_order_release);
            impl_->shuttingDown.store(false, std::memory_order_release);
            return;
        }

        {
            std::lock_guard lock(impl_->shellMutex);
            if (!impl_->shellViews.empty()) {
                logger::info("Clearing {} CEF shell view states during shutdown.", impl_->shellViews.size());
            }
            impl_->shellViews.clear();
            impl_->shellReady = false;
            impl_->shellFrameIdentifier.clear();
            impl_->shellUrl.clear();
        }

        {
            std::lock_guard lock(impl_->devToolsMutex);
            impl_->devToolsBrowser = nullptr;
            impl_->devToolsClient = nullptr;
        }
        impl_->devToolsOpen.store(false, std::memory_order_release);
        impl_->devToolsTargetBrowserId.store(-1, std::memory_order_release);
        impl_->devToolsBrowserId.store(-1, std::memory_order_release);

        logger::info("Calling CefShutdown.");
        CefShutdown();
        logger::info("CefShutdown completed.");

        {
            std::lock_guard lock(impl_->stateMutex);
            impl_->client = nullptr;
            impl_->app = nullptr;
            impl_->width = 0;
            impl_->height = 0;
            impl_->hwnd = nullptr;
            impl_->initialized.store(false, std::memory_order_release);
            impl_->shuttingDown.store(false, std::memory_order_release);
        }
    }

    bool CefRuntime::IsInitialized() const { return impl_->initialized.load(std::memory_order_acquire); }

    bool CefRuntime::HasBrowser() const {
        std::lock_guard lock(impl_->stateMutex);
        return impl_->client && impl_->client->HasBrowser();
    }

    void CefRuntime::PostToCefUi(std::function<void()> task) {
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            return;
        }

        if (CefCurrentlyOn(TID_UI)) {
            task();
            return;
        }

        if (!CefPostTask(TID_UI, new FunctionTask(std::move(task)))) {
            logger::error("Failed to post task to CEF UI thread.");
        }
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

    void CefRuntime::DispatchInputEvents(uint64_t viewId, std::vector<CefInputEvent> events) {
        if (events.empty()) {
            logger::debug("CEF input dispatch ignored empty batch for View [{}].", viewId);
            return;
        }

        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: runtime unavailable.", events.size(),
                         viewId);
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: browser unavailable.", events.size(),
                         viewId);
            return;
        }

        PostToCefUi([this, client, viewId, events = std::move(events)]() mutable {
            std::string iframeName;
            {
                std::lock_guard lock(impl_->shellMutex);
                const auto it = impl_->shellViews.find(viewId);
                if (it == impl_->shellViews.end()) {
                    logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: missing shell view.",
                                 events.size(), viewId);
                    return;
                }
                if (it->second.hidden) {
                    logger::debug("CEF input dispatch dropped {} event(s) for hidden View [{}].", events.size(),
                                  viewId);
                    return;
                }
                if (!impl_->shellReady) {
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
                    [host, viewId](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
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

    void CefRuntime::InvokeScript(uint64_t viewId, std::string script, std::function<void(std::string)> callback) {
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("InvokeScript: CEF not initialized; firing empty callback for view [{}].", viewId);
            if (callback) callback(std::string());
            return;
        }

        const uint64_t requestId = impl_->nextRequestId.fetch_add(1, std::memory_order_relaxed);

        if (callback) {
            std::lock_guard lock(impl_->invokeMutex);
            Impl::InvokeEntry entry;
            entry.viewId = viewId;
            entry.callback = std::move(callback);
            impl_->pendingInvokes.emplace(requestId, std::move(entry));
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
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
                    std::lock_guard lock(impl_->invokeMutex);
                    auto it = impl_->pendingInvokes.find(requestId);
                    if (it != impl_->pendingInvokes.end()) {
                        drained = std::move(it->second);
                        impl_->pendingInvokes.erase(it);
                        have = true;
                    }
                }
                if (have && drained.callback) drained.callback(std::string());
                return;
            }
            logger::debug("InvokeScript: dispatching request {} to view [{}].", requestId, viewId);
            Messaging::SendProcessMessageToFrame(
                frame, PID_RENDERER, BTRMessages::InvokeRequestMessage{.RequestId = requestId, .Script = scriptCopy});
        });
    }

    void CefRuntime::InteropCallInView(uint64_t viewId, std::string functionName, std::string argument) {
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("InteropCall: CEF not initialized; ignoring call to '{}' on view [{}].", functionName, viewId);
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
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
                                      std::function<void(const std::string&)> /*callback*/) {
        // The callback itself lives in Core::jsCallbacks; this method only forwards
        // the "install trampoline" message to the renderer so the iframe exposes a
        // window[name] = function(arg) bridge. Caller (Communication::RegisterJSListener)
        // is responsible for storing the callback first.
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("RegisterListener: CEF not initialized; '{}' for view [{}] will be installed lazily.", name,
                         viewId);
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
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

    void CefRuntime::CancelInvokesForView(uint64_t viewId) {
        std::vector<Impl::InvokeEntry> drained;
        {
            std::lock_guard lock(impl_->invokeMutex);
            for (auto it = impl_->pendingInvokes.begin(); it != impl_->pendingInvokes.end();) {
                if (it->second.viewId == viewId) {
                    drained.push_back(std::move(it->second));
                    it = impl_->pendingInvokes.erase(it);
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
                                       const RTBMessages::RendererToBrowserMessage& message) {
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
                    std::lock_guard lock(impl_->invokeMutex);
                    auto it = impl_->pendingInvokes.find(m.RequestId);
                    if (it != impl_->pendingInvokes.end()) {
                        entry = std::move(it->second);
                        impl_->pendingInvokes.erase(it);
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
