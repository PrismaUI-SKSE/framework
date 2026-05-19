#include "PCH.h"

#ifdef GetNextSibling
#    undef GetNextSibling
#endif

#include "Cef/CefRuntime.h"
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <type_traits>
#include <vector>

#include "Cef/CefOsrClient.h"
#include "Cef/PrismaCefApp.h"
#include "Cef/ProcessMessageNames.h"
#include "PrismaUI/Communication.h"
#include "Utils/DllLoader.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_process_message.h"
#include "include/cef_task.h"
#include "include/cef_values.h"

namespace
{
    constexpr int kCefWindowlessFrameRate = 120;
    constexpr std::chrono::milliseconds kBrowserCloseTimeout{5000};

    enum class OverlayMode : uint8_t
    {
        None,
        Accelerated,
        Cpu
    };

    struct OverlayDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

        bool Matches(const D3D11_TEXTURE2D_DESC& desc) const
        {
            return width == desc.Width && height == desc.Height && format == desc.Format;
        }
    };

    const char* OverlayModeName(OverlayMode mode)
    {
        switch (mode) {
            case OverlayMode::Accelerated:
                return "accelerated shared texture";
            case OverlayMode::Cpu:
                return "CPU OnPaint fallback";
            default:
                return "none";
        }
    }

    std::wstring ToFileUrl(const std::filesystem::path& path)
    {
        std::wstring generic = std::filesystem::absolute(path).generic_wstring();
        if (!generic.empty() && generic.front() != L'/') {
            generic.insert(generic.begin(), L'/');
        }
        return L"file://" + generic;
    }

    std::string NarrowAscii(const std::wstring& value)
    {
        std::string result;
        result.reserve(value.size());
        for (const wchar_t ch : value) {
            result.push_back(ch >= 0 && ch <= 0x7F ? static_cast<char>(ch) : '?');
        }
        return result;
    }

    std::string MakeIframeName(uint64_t viewId)
    {
        return "prisma-view-" + std::to_string(viewId);
    }

    bool StartsWithInsensitive(std::string_view value, std::string_view prefix)
    {
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

    bool IsAbsoluteBrowserUrl(std::string_view value)
    {
        return StartsWithInsensitive(value, "http://") || StartsWithInsensitive(value, "https://") ||
               StartsWithInsensitive(value, "file://") || StartsWithInsensitive(value, "about:") ||
               StartsWithInsensitive(value, "data:");
    }

    std::string ResolveViewUrl(std::string_view urlOrPath)
    {
        if (IsAbsoluteBrowserUrl(urlOrPath)) {
            return std::string(urlOrPath);
        }

        std::filesystem::path path{std::string(urlOrPath)};
        if (path.is_relative()) {
            path = PrismaUI::Utils::GetBasePath() / "views" / path;
        }
        return NarrowAscii(ToFileUrl(path));
    }

    std::string JsonEscape(std::string_view value)
    {
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

    bool TryParseIframeViewId(std::string_view frameName, uint64_t& viewId)
    {
        constexpr std::string_view kPrefix = "prisma-view-";
        if (frameName.size() <= kPrefix.size() || frameName.substr(0, kPrefix.size()) != kPrefix) {
            return false;
        }

        uint64_t value = 0;
        for (const char ch : frameName.substr(kPrefix.size())) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            const uint64_t digit = static_cast<uint64_t>(ch - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
                return false;
            }
            value = value * 10U + digit;
        }

        viewId = value;
        return true;
    }

    class FunctionTask final : public CefTask
    {
    public:
        explicit FunctionTask(std::function<void()> task) : task_(std::move(task)) {}

        void Execute() override
        {
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
}

namespace PrismaUI::Cef
{
    struct CefRuntime::Impl
    {
        mutable std::mutex stateMutex;
        CefRefPtr<CefApp> app;
        CefRefPtr<CefOsrClient> client;
        std::atomic<bool> initializeAttempted = false;
        std::atomic<bool> initialized = false;
        std::atomic<bool> shuttingDown = false;
        std::atomic<bool> shutdownSkipped = false;
        uint32_t width = 0;
        uint32_t height = 0;

        struct ShellViewState
        {
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

        mutable std::mutex overlayMutex;
        Microsoft::WRL::ComPtr<ID3D11Device> renderDevice;
        Microsoft::WRL::ComPtr<ID3D11Device1> renderDevice1;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> renderContext;
        Microsoft::WRL::ComPtr<ID3D11Multithread> d3dMultithread;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> overlayTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overlaySrv;
        OverlayDesc overlayDesc;
        OverlayDesc pendingAcceleratedDesc;
        OverlayMode overlayMode = OverlayMode::None;
        OverlayMode activeMode = OverlayMode::None;
        bool pendingAcceleratedTexture = false;
        bool overlayHasFrame = false;
        bool multithreadProtectionConfigured = false;
        bool multithreadProtectionUnavailableLogged = false;
        bool missingD3DLogged = false;
        bool firstAcceleratedCopyLogged = false;

        // ---- Step 7 JS bridge state ----
        struct InvokeEntry
        {
            uint64_t viewId = 0;
            std::function<void(std::string)> callback;
        };
        std::mutex invokeMutex;
        std::atomic<uint64_t> nextRequestId = 1;
        std::map<uint64_t, InvokeEntry> pendingInvokes;
    };

    CefRuntime::CefRuntime() : impl_(std::make_unique<Impl>()) {}

    CefRuntime::~CefRuntime() = default;

    CefRuntime& CefRuntime::GetSingleton()
    {
        static CefRuntime instance;
        return instance;
    }

    bool CefRuntime::Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width,
                                uint32_t height)
    {
        std::lock_guard lock(impl_->stateMutex);

        if (impl_->initialized.load(std::memory_order_acquire)) {
            return true;
        }

        if (impl_->initializeAttempted.exchange(true, std::memory_order_acq_rel)) {
            logger::debug("CEF initialization was already attempted and did not complete successfully.");
            return false;
        }

        if (!hwnd || !device || !context || width == 0 || height == 0) {
            logger::error("CEF initialization skipped: hwnd={}, device={}, context={}, size={}x{}", hwnd ? "set" : "null",
                          device ? "set" : "null", context ? "set" : "null", width, height);
            return false;
        }

        logger::info("Initializing CEF runtime for PrismaUI lifecycle smoke browser.");

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
            const bool requested = CefBrowserHost::CreateBrowser(windowInfo, client, url, browserSettings, nullptr, nullptr);
            if (!requested) {
                logger::error("CefBrowserHost::CreateBrowser returned false.");
            }
        });

        return true;
    }

    void CefRuntime::Resize(uint32_t width, uint32_t height)
    {
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

    void CefRuntime::BeginFrame()
    {
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

    void CefRuntime::UpdateOverlayTexture(ID3D11Device* device, ID3D11DeviceContext* context)
    {
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
            std::lock_guard lock(impl_->overlayMutex);
            if (!impl_->missingD3DLogged) {
                logger::warn("CEF overlay texture update skipped: D3D device/context is missing.");
                impl_->missingD3DLogged = true;
            }
            return;
        }

        std::vector<std::byte> cpuFrame;
        uint32_t cpuWidth = 0;
        uint32_t cpuHeight = 0;
        uint32_t cpuStride = 0;
        const bool hasCpuFrame = client->ConsumeCpuFrame(cpuFrame, cpuWidth, cpuHeight, cpuStride);

        std::lock_guard lock(impl_->overlayMutex);
        impl_->missingD3DLogged = false;

        if (impl_->renderDevice.Get() != device) {
            impl_->renderDevice = device;
            impl_->renderDevice1.Reset();
            const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(impl_->renderDevice1.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                logger::error("CEF accelerated OSR disabled: D3D device does not expose ID3D11Device1. HR={:#X}", hr);
            }
        }

        if (impl_->renderContext.Get() != context) {
            impl_->renderContext = context;
            impl_->d3dMultithread.Reset();
            impl_->multithreadProtectionConfigured = false;
        }

        if (!impl_->multithreadProtectionConfigured) {
            const HRESULT hr = context->QueryInterface(IID_PPV_ARGS(impl_->d3dMultithread.ReleaseAndGetAddressOf()));
            if (SUCCEEDED(hr) && impl_->d3dMultithread) {
                const BOOL wasProtected = impl_->d3dMultithread->SetMultithreadProtected(TRUE);
                logger::info("CEF overlay enabled D3D11 multithread protection (previously {}).",
                             wasProtected ? "enabled" : "disabled");
            } else if (!impl_->multithreadProtectionUnavailableLogged) {
                logger::warn("CEF overlay could not acquire ID3D11Multithread; accelerated copies will use only the overlay mutex. HR={:#X}",
                             hr);
                impl_->multithreadProtectionUnavailableLogged = true;
            }
            impl_->multithreadProtectionConfigured = true;
        }

        if (impl_->pendingAcceleratedTexture && impl_->pendingAcceleratedDesc.width != 0 &&
            impl_->pendingAcceleratedDesc.height != 0 && impl_->pendingAcceleratedDesc.format != DXGI_FORMAT_UNKNOWN) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = impl_->pendingAcceleratedDesc.width;
            desc.Height = impl_->pendingAcceleratedDesc.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = impl_->pendingAcceleratedDesc.format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                logger::error("Failed to create accelerated CEF overlay texture {}x{} format {}. HR={:#X}",
                              desc.Width, desc.Height, static_cast<unsigned int>(desc.Format), hr);
                impl_->pendingAcceleratedTexture = false;
            } else {
                hr = device->CreateShaderResourceView(texture.Get(), nullptr, srv.ReleaseAndGetAddressOf());
                if (FAILED(hr)) {
                    logger::error("Failed to create accelerated CEF overlay SRV {}x{} format {}. HR={:#X}",
                                  desc.Width, desc.Height, static_cast<unsigned int>(desc.Format), hr);
                    impl_->pendingAcceleratedTexture = false;
                } else {
                    impl_->overlayTexture = std::move(texture);
                    impl_->overlaySrv = std::move(srv);
                    impl_->overlayDesc = impl_->pendingAcceleratedDesc;
                    impl_->overlayMode = OverlayMode::Accelerated;
                    impl_->overlayHasFrame = false;
                    impl_->pendingAcceleratedTexture = false;
                    logger::info("Created accelerated CEF overlay texture {}x{} DXGI format {}.",
                                 impl_->overlayDesc.width, impl_->overlayDesc.height,
                                 static_cast<unsigned int>(impl_->overlayDesc.format));
                    PostToCefUi([client]() {
                        client->InvalidateView();
                        client->SendExternalBeginFrame();
                    });
                }
            }
        }

        if (!hasCpuFrame) {
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

        const OverlayDesc cpuDesc{cpuWidth, cpuHeight, DXGI_FORMAT_B8G8R8A8_UNORM};
        if (!impl_->overlayTexture || !impl_->overlaySrv || impl_->overlayMode != OverlayMode::Cpu ||
            impl_->overlayDesc.width != cpuDesc.width || impl_->overlayDesc.height != cpuDesc.height ||
            impl_->overlayDesc.format != cpuDesc.format) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = cpuDesc.width;
            desc.Height = cpuDesc.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = cpuDesc.format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                logger::error("Failed to create CPU fallback CEF overlay texture {}x{}. HR={:#X}", desc.Width,
                              desc.Height, hr);
                return;
            }

            hr = device->CreateShaderResourceView(texture.Get(), nullptr, srv.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                logger::error("Failed to create CPU fallback CEF overlay SRV {}x{}. HR={:#X}", desc.Width,
                              desc.Height, hr);
                return;
            }

            impl_->overlayTexture = std::move(texture);
            impl_->overlaySrv = std::move(srv);
            impl_->overlayDesc = cpuDesc;
            impl_->overlayMode = OverlayMode::Cpu;
            impl_->overlayHasFrame = false;
            logger::warn("Created degraded CPU fallback CEF overlay texture {}x{}.", cpuDesc.width, cpuDesc.height);
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map(impl_->overlayTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            logger::error("Failed to map CPU fallback CEF overlay texture. HR={:#X}", hr);
            return;
        }

        auto* destination = static_cast<std::byte*>(mapped.pData);
        const std::byte* source = cpuFrame.data();
        for (uint32_t y = 0; y < cpuHeight; ++y) {
            std::memcpy(destination + static_cast<size_t>(y) * mapped.RowPitch,
                        source + static_cast<size_t>(y) * cpuStride, rowBytes);
        }
        context->Unmap(impl_->overlayTexture.Get(), 0);

        impl_->overlayHasFrame = true;
        if (impl_->activeMode != OverlayMode::Cpu) {
            logger::warn("CEF render path switched: {} -> {}.", OverlayModeName(impl_->activeMode),
                         OverlayModeName(OverlayMode::Cpu));
            impl_->activeMode = OverlayMode::Cpu;
        }
    }

    ID3D11ShaderResourceView* CefRuntime::GetOverlaySrv() const
    {
        std::lock_guard lock(impl_->overlayMutex);
        return impl_->overlayHasFrame ? impl_->overlaySrv.Get() : nullptr;
    }

    uint32_t CefRuntime::GetOverlayWidth() const
    {
        std::lock_guard lock(impl_->overlayMutex);
        return impl_->overlayHasFrame ? impl_->overlayDesc.width : 0;
    }

    uint32_t CefRuntime::GetOverlayHeight() const
    {
        std::lock_guard lock(impl_->overlayMutex);
        return impl_->overlayHasFrame ? impl_->overlayDesc.height : 0;
    }

    void CefRuntime::ReleaseRenderResources()
    {
        std::lock_guard lock(impl_->overlayMutex);
        if (impl_->overlayTexture || impl_->overlaySrv || impl_->renderDevice || impl_->renderContext) {
            logger::info("Releasing CEF overlay D3D resources.");
        }
        impl_->overlaySrv.Reset();
        impl_->overlayTexture.Reset();
        impl_->renderContext.Reset();
        impl_->renderDevice1.Reset();
        impl_->renderDevice.Reset();
        impl_->d3dMultithread.Reset();
        impl_->overlayDesc = {};
        impl_->pendingAcceleratedDesc = {};
        impl_->overlayMode = OverlayMode::None;
        impl_->activeMode = OverlayMode::None;
        impl_->pendingAcceleratedTexture = false;
        impl_->overlayHasFrame = false;
        impl_->multithreadProtectionConfigured = false;
        impl_->missingD3DLogged = false;
    }

    bool CefRuntime::CopyAcceleratedFrameDuringCallback(HANDLE sharedTextureHandle)
    {
        if (!sharedTextureHandle) {
            return false;
        }

        std::lock_guard lock(impl_->overlayMutex);
        if (!impl_->renderDevice1 || !impl_->renderContext) {
            if (!impl_->missingD3DLogged) {
                logger::warn("CEF accelerated paint arrived before the D3D11.1 render bridge was ready.");
                impl_->missingD3DLogged = true;
            }
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
        HRESULT hr = impl_->renderDevice1->OpenSharedResource1(sharedTextureHandle, IID_PPV_ARGS(sharedTexture.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            logger::error("Failed to open CEF accelerated shared texture. HR={:#X}", hr);
            return false;
        }

        D3D11_TEXTURE2D_DESC sharedDesc = {};
        sharedTexture->GetDesc(&sharedDesc);
        if (sharedDesc.Width == 0 || sharedDesc.Height == 0 || sharedDesc.Format == DXGI_FORMAT_UNKNOWN) {
            logger::error("CEF accelerated shared texture had invalid description {}x{} format {}.", sharedDesc.Width,
                          sharedDesc.Height, static_cast<unsigned int>(sharedDesc.Format));
            return false;
        }

        const OverlayDesc incoming{sharedDesc.Width, sharedDesc.Height, sharedDesc.Format};
        const bool overlayMatches = impl_->overlayTexture && impl_->overlaySrv &&
                                    impl_->overlayMode == OverlayMode::Accelerated &&
                                    impl_->overlayDesc.Matches(sharedDesc);
        if (!overlayMatches) {
            const bool pendingMatches = impl_->pendingAcceleratedTexture &&
                                        impl_->pendingAcceleratedDesc.width == incoming.width &&
                                        impl_->pendingAcceleratedDesc.height == incoming.height &&
                                        impl_->pendingAcceleratedDesc.format == incoming.format;
            if (!pendingMatches) {
                impl_->pendingAcceleratedDesc = incoming;
                impl_->pendingAcceleratedTexture = true;
                logger::info("CEF accelerated shared texture description requested {}x{} DXGI format {}.",
                             incoming.width, incoming.height, static_cast<unsigned int>(incoming.format));
            }
            return false;
        }

        impl_->renderContext->CopyResource(impl_->overlayTexture.Get(), sharedTexture.Get());
        impl_->overlayHasFrame = true;
        if (!impl_->firstAcceleratedCopyLogged) {
            logger::info("First accelerated CEF overlay frame copied: {}x{} DXGI format {}.", impl_->overlayDesc.width,
                         impl_->overlayDesc.height, static_cast<unsigned int>(impl_->overlayDesc.format));
            impl_->firstAcceleratedCopyLogged = true;
        }
        if (impl_->activeMode != OverlayMode::Accelerated) {
            logger::info("CEF render path switched: {} -> {}.", OverlayModeName(impl_->activeMode),
                         OverlayModeName(OverlayMode::Accelerated));
            impl_->activeMode = OverlayMode::Accelerated;
        }
        return true;
    }

    bool CefRuntime::RunShellCommand(const std::string& command, const std::string& description,
                                     const std::string& iframeName)
    {
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF shell command '{}' ignored because CEF is not initialized.", description);
            return false;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF shell command '{}' ignored because no browser is available.", description);
            return false;
        }

        {
            std::lock_guard lock(impl_->shellMutex);
            if (!impl_->shellReady) {
                logger::warn("CEF shell command '{}' deferred because the shell is not ready.", description);
                return false;
            }
        }

        PostToCefUi([client, command, description, iframeName]() {
            CefRefPtr<CefBrowser> browser = client->GetBrowserOnUiThread();
            if (!browser) {
                logger::error("CEF shell command '{}' failed: browser disappeared.", description);
                return;
            }

            if (!iframeName.empty()) {
                CefString frameName;
                frameName.FromString(iframeName);
                if (!client->GetFrameByNameOnUiThread(frameName)) {
                    logger::warn("CEF shell command '{}' did not find iframe frame '{}'.", description, iframeName);
                }
            }

            CefRefPtr<CefFrame> mainFrame = browser->GetMainFrame();
            if (!mainFrame) {
                logger::error("CEF shell command '{}' failed: main frame is unavailable.", description);
                return;
            }

            CefString sourceUrl;
            sourceUrl.FromASCII("prismaui://shell-command");
            CefString script;
            script.FromString(command);
            mainFrame->ExecuteJavaScript(script, sourceUrl, 0);
            logger::debug("CEF shell command '{}' executed.", description);
        });
        return true;
    }

    void CefRuntime::ReplayShellViews()
    {
        std::vector<Impl::ShellViewState> views;
        {
            std::lock_guard lock(impl_->shellMutex);
            views.reserve(impl_->shellViews.size());
            for (const auto& [viewId, state] : impl_->shellViews) {
                views.push_back(state);
            }
        }

        for (const auto& state : views) {
            std::string command = "window.__prismaShell.createView({ id: " + JsonEscape(std::to_string(state.viewId)) +
                                  ", url: " + JsonEscape(state.resolvedUrl) +
                                  ", order: " + std::to_string(state.order) +
                                  ", hidden: " + (state.hidden ? "true" : "false") + " });";
            RunShellCommand(command, "replay createView " + state.iframeName);
            if (state.focused) {
                command = "window.__prismaShell.focusView(" + JsonEscape(std::to_string(state.viewId)) + ");";
                RunShellCommand(command, "replay focusView " + state.iframeName, state.iframeName);
            }
        }
    }

    bool CefRuntime::CreateShellView(uint64_t viewId, std::string_view urlOrPath, int order, bool hidden)
    {
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

        logger::info("CEF shell create view: id={}, iframe='{}', url='{}', order={}, hidden={}.",
                     viewId, iframeName, resolvedUrl, order, hidden);
        const std::string command = "window.__prismaShell.createView({ id: " + JsonEscape(std::to_string(viewId)) +
                                    ", url: " + JsonEscape(resolvedUrl) + ", order: " + std::to_string(order) +
                                    ", hidden: " + (hidden ? "true" : "false") + " });";
        return RunShellCommand(command, "createView " + iframeName) || (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::DestroyShellView(uint64_t viewId)
    {
        const std::string iframeName = MakeIframeName(viewId);
        bool existed = false;
        {
            std::lock_guard lock(impl_->shellMutex);
            existed = impl_->shellViews.erase(viewId) != 0;
        }

        logger::info("CEF shell destroy view: id={}, iframe='{}', existed={}.", viewId, iframeName, existed);
        const std::string command = "window.__prismaShell.destroyView(" + JsonEscape(std::to_string(viewId)) + ");";
        if (!existed) {
            return true;
        }
        return RunShellCommand(command, "destroyView " + iframeName, iframeName) ||
               (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::SetShellViewHidden(uint64_t viewId, bool hidden)
    {
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
        const std::string command = "window.__prismaShell.setHidden(" + JsonEscape(std::to_string(viewId)) + ", " +
                                    (hidden ? "true" : "false") + ");";
        return RunShellCommand(command, "setHidden " + iframeName, iframeName) ||
               (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::SetShellViewOrder(uint64_t viewId, int order)
    {
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
        const std::string command = "window.__prismaShell.setOrder(" + JsonEscape(std::to_string(viewId)) + ", " +
                                    std::to_string(order) + ");";
        return RunShellCommand(command, "setOrder " + iframeName, iframeName) ||
               (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::FocusShellView(uint64_t viewId)
    {
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
        const std::string command = "window.__prismaShell.focusView(" + JsonEscape(std::to_string(viewId)) + ");";
        return RunShellCommand(command, "focusView " + iframeName, iframeName) ||
               (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::BlurShellView(uint64_t viewId)
    {
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
        const std::string command = "window.__prismaShell.blurView(" + JsonEscape(std::to_string(viewId)) + ");";
        return RunShellCommand(command, "blurView " + iframeName, iframeName) ||
               (IsInitialized() && HasBrowser() && !IsShellReady());
    }

    bool CefRuntime::TryGetShellFrameName(uint64_t viewId, std::string& outName) const
    {
        std::lock_guard lock(impl_->shellMutex);
        const auto it = impl_->shellViews.find(viewId);
        if (it == impl_->shellViews.end()) {
            outName.clear();
            return false;
        }
        outName = it->second.iframeName;
        return true;
    }

    bool CefRuntime::IsShellReady() const
    {
        std::lock_guard lock(impl_->shellMutex);
        return impl_->shellReady;
    }

    void CefRuntime::NotifyShellLoadStart(const std::string& frameIdentifier, const std::string& url)
    {
        std::lock_guard lock(impl_->shellMutex);
        impl_->shellReady = false;
        impl_->shellFrameIdentifier = frameIdentifier;
        impl_->shellUrl = url;
        logger::info("CEF shell state: load start, frame id '{}', url '{}'.", frameIdentifier, url);
    }

    void CefRuntime::NotifyShellLoadEnd(int httpStatusCode, const std::string& frameIdentifier, const std::string& url)
    {
        {
            std::lock_guard lock(impl_->shellMutex);
            impl_->shellReady = true;
            impl_->shellFrameIdentifier = frameIdentifier;
            impl_->shellUrl = url;
        }
        logger::info("CEF shell state: ready, frame id '{}', status {}, url '{}'.", frameIdentifier, httpStatusCode, url);
        ReplayShellViews();
    }

    void CefRuntime::NotifyShellLoadError(int errorCode, const std::string& errorText, const std::string& failedUrl,
                                          const std::string& frameIdentifier, const std::string& url)
    {
        std::lock_guard lock(impl_->shellMutex);
        impl_->shellReady = false;
        impl_->shellFrameIdentifier = frameIdentifier;
        impl_->shellUrl = url;
        logger::error("CEF shell state: load error code={}, error='{}', failedUrl='{}', frame id '{}', url '{}'.",
                      errorCode, errorText, failedUrl, frameIdentifier, url);
    }

    void CefRuntime::NotifyShellFrameLoadStart(const std::string& frameName, const std::string& frameIdentifier,
                                               const std::string& url)
    {
        uint64_t viewId = 0;
        if (!TryParseIframeViewId(frameName, viewId)) {
            logger::warn("CEF shell could not correlate iframe load start: frame='{}', id='{}', url='{}'.",
                         frameName, frameIdentifier, url);
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
                                             const std::string& url, int httpStatusCode)
    {
        uint64_t viewId = 0;
        if (!TryParseIframeViewId(frameName, viewId)) {
            logger::warn("CEF shell could not correlate iframe load end: frame='{}', id='{}', status {}, url='{}'.",
                         frameName, frameIdentifier, httpStatusCode, url);
            return;
        }

        std::lock_guard lock(impl_->shellMutex);
        auto it = impl_->shellViews.find(viewId);
        if (it == impl_->shellViews.end()) {
            logger::warn("CEF shell iframe load end for unknown view: id={}, iframe='{}', frame id '{}', status {}, url='{}'.",
                         viewId, frameName, frameIdentifier, httpStatusCode, url);
            return;
        }
        it->second.loadState = "loaded";
        it->second.lastFrameIdentifier = frameIdentifier;
        it->second.lastFrameName = frameName;
        logger::info("CEF shell iframe state: id={}, iframe='{}', loaded status {}, url='{}'.",
                     viewId, frameName, httpStatusCode, url);
    }

    void CefRuntime::NotifyShellFrameLoadError(const std::string& frameName, const std::string& frameIdentifier,
                                               const std::string& url, int errorCode, const std::string& errorText,
                                               const std::string& failedUrl)
    {
        uint64_t viewId = 0;
        if (!TryParseIframeViewId(frameName, viewId)) {
            logger::warn("CEF shell could not correlate iframe load error: frame='{}', id='{}', code={}, failedUrl='{}'.",
                         frameName, frameIdentifier, errorCode, failedUrl);
            return;
        }

        std::lock_guard lock(impl_->shellMutex);
        auto it = impl_->shellViews.find(viewId);
        if (it == impl_->shellViews.end()) {
            logger::warn("CEF shell iframe load error for unknown view: id={}, iframe='{}', frame id '{}', code={}, failedUrl='{}'.",
                         viewId, frameName, frameIdentifier, errorCode, failedUrl);
            return;
        }
        it->second.loadState = "error";
        it->second.lastFrameIdentifier = frameIdentifier;
        it->second.lastFrameName = frameName;
        logger::error("CEF shell iframe state: id={}, iframe='{}', load error code={}, error='{}', failedUrl='{}', url='{}'.",
                      viewId, frameName, errorCode, errorText, failedUrl, url);
    }

    void CefRuntime::Shutdown()
    {
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
            PostToCefUi([client]() { client->CloseBrowser(); });
            browserClosed = client->WaitForClose(kBrowserCloseTimeout);
            if (browserClosed) {
                logger::info("CEF browser closed before shutdown.");
            } else {
                logger::error("Timed out waiting for CEF browser close; skipping CefShutdown to avoid a shutdown deadlock.");
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

        logger::info("Calling CefShutdown.");
        CefShutdown();
        logger::info("CefShutdown completed.");

        {
            std::lock_guard lock(impl_->stateMutex);
            impl_->client = nullptr;
            impl_->app = nullptr;
            impl_->width = 0;
            impl_->height = 0;
            impl_->initialized.store(false, std::memory_order_release);
            impl_->shuttingDown.store(false, std::memory_order_release);
        }
    }

    bool CefRuntime::IsInitialized() const
    {
        return impl_->initialized.load(std::memory_order_acquire);
    }

    bool CefRuntime::HasBrowser() const
    {
        std::lock_guard lock(impl_->stateMutex);
        return impl_->client && impl_->client->HasBrowser();
    }

    void CefRuntime::PostToCefUi(std::function<void()> task)
    {
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
        cef_key_event_type_t ToCefKeyType(CefInputKeyType type)
        {
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

        cef_mouse_button_type_t ToCefMouseButton(CefInputMouseButton button)
        {
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

    void CefRuntime::DispatchInputEvents(uint64_t viewId, std::vector<CefInputEvent> events)
    {
        if (events.empty()) {
            logger::debug("CEF input dispatch ignored empty batch for View [{}].", viewId);
            return;
        }

        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: runtime unavailable.",
                         events.size(), viewId);
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
        }

        if (!client || !client->HasBrowser()) {
            logger::warn("CEF input dispatch dropped {} event(s) for View [{}]: browser unavailable.",
                         events.size(), viewId);
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
                    logger::debug("CEF input dispatch dropped {} event(s) for hidden View [{}].",
                                  events.size(), viewId);
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

            if (CefRefPtr<CefFrame> mainFrame = browser->GetMainFrame()) {
                const std::string command =
                    "window.__prismaShell.focusView(" + JsonEscape(std::to_string(viewId)) + ");";
                CefString sourceUrl;
                sourceUrl.FromASCII("prismaui://input-focus");
                CefString script;
                script.FromString(command);
                mainFrame->ExecuteJavaScript(script, sourceUrl, 0);
            } else {
                logger::warn("CEF input dispatch for View [{}] could not refresh iframe focus: main frame missing.",
                             viewId);
            }

            logger::debug("CEF input dispatch sending {} event(s) to View [{}].", events.size(), viewId);
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
                            host->SendMouseClickEvent(mouseEvent, ToCefMouseButton(value.button),
                                                       value.mouseUp, value.clickCount);
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

    // ============== Step 7 JS bridge ==============

    namespace {
        CefRefPtr<CefProcessMessage> MakeStringListMessage(const char* name,
                                                          std::initializer_list<std::string> args)
        {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(name);
            CefRefPtr<CefListValue> list = msg->GetArgumentList();
            list->SetSize(args.size());
            size_t i = 0;
            for (const auto& a : args) {
                list->SetString(i++, a);
            }
            return msg;
        }

        // Find the iframe frame on the CEF UI thread. Returns nullptr if the iframe
        // does not (yet) exist — caller is responsible for logging.
        CefRefPtr<CefFrame> FindIframeFrame(CefRefPtr<CefOsrClient> client, uint64_t viewId)
        {
            if (!client) return nullptr;
            const std::string iframeName = "prisma-view-" + std::to_string(viewId);
            CefString frameName;
            frameName.FromString(iframeName);
            return client->GetFrameByNameOnUiThread(frameName);
        }
    }

    void CefRuntime::InvokeScript(uint64_t viewId, std::string script,
                                  std::function<void(std::string)> callback)
    {
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

        const std::string requestIdStr = std::to_string(requestId);
        const std::string scriptCopy = std::move(script);

        PostToCefUi([this, client, viewId, requestIdStr, scriptCopy]() {
            CefRefPtr<CefFrame> frame = FindIframeFrame(client, viewId);
            if (!frame) {
                logger::warn("InvokeScript: iframe for view [{}] not yet attached; failing request {}.",
                             viewId, requestIdStr);
                // Fire the queued callback with an empty string and remove the entry.
                Impl::InvokeEntry drained;
                bool have = false;
                {
                    std::lock_guard lock(impl_->invokeMutex);
                    auto it = impl_->pendingInvokes.find(std::stoull(requestIdStr));
                    if (it != impl_->pendingInvokes.end()) {
                        drained = std::move(it->second);
                        impl_->pendingInvokes.erase(it);
                        have = true;
                    }
                }
                if (have && drained.callback) drained.callback(std::string());
                return;
            }
            logger::debug("InvokeScript: dispatching request {} to view [{}].", requestIdStr, viewId);
            frame->SendProcessMessage(PID_RENDERER,
                                      MakeStringListMessage(Messages::kInvokeRequest,
                                                            {requestIdStr, scriptCopy}));
        });
    }

    void CefRuntime::InteropCallInView(uint64_t viewId, std::string functionName, std::string argument)
    {
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("InteropCall: CEF not initialized; ignoring call to '{}' on view [{}].",
                         functionName, viewId);
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
                logger::warn("InteropCall: iframe for view [{}] is not attached; dropping call to '{}'.",
                             viewId, fn);
                return;
            }
            frame->SendProcessMessage(PID_RENDERER,
                                      MakeStringListMessage(Messages::kInteropCall, {fn, arg}));
        });
    }

    void CefRuntime::RegisterListener(uint64_t viewId, std::string name,
                                      std::function<void(const std::string&)> /*callback*/)
    {
        // The callback itself lives in Core::jsCallbacks; this method only forwards
        // the "install trampoline" message to the renderer so the iframe exposes a
        // window[name] = function(arg) bridge. Caller (Communication::RegisterJSListener)
        // is responsible for storing the callback first.
        if (!impl_->initialized.load(std::memory_order_acquire)) {
            logger::warn("RegisterListener: CEF not initialized; '{}' for view [{}] will be installed lazily.",
                         name, viewId);
            return;
        }

        CefRefPtr<CefOsrClient> client;
        {
            std::lock_guard lock(impl_->stateMutex);
            client = impl_->client;
        }

        const std::string viewIdStr = std::to_string(viewId);
        const std::string nameCopy = std::move(name);

        PostToCefUi([client, viewId, viewIdStr, nameCopy]() {
            CefRefPtr<CefFrame> frame = FindIframeFrame(client, viewId);
            if (!frame) {
                // No frame yet — renderer will queue installs at OnContextCreated once
                // the iframe lands. We still try once here; if the frame appears later
                // the listener is re-registered when the iframe re-creates its context.
                logger::info("RegisterListener: iframe for view [{}] not yet attached; '{}' will install at next context.",
                             viewId, nameCopy);
                return;
            }
            logger::info("RegisterListener: installing '{}' for view [{}].", nameCopy, viewId);
            frame->SendProcessMessage(PID_RENDERER,
                                      MakeStringListMessage(Messages::kInstallListener,
                                                            {viewIdStr, nameCopy}));
        });
    }

    void CefRuntime::CancelInvokesForView(uint64_t viewId)
    {
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
            logger::info("CancelInvokesForView: draining {} pending Invoke callback(s) for view [{}].",
                         drained.size(), viewId);
        }
        for (auto& entry : drained) {
            if (entry.callback) entry.callback(std::string());
        }
    }

    bool CefRuntime::OnRendererMessage(const std::string& frameName, const std::string& messageName,
                                       const std::vector<std::string>& payload)
    {
        // Pull viewId out of either the frame name ("prisma-view-<id>") or the
        // first payload column, depending on which message kind we're handling.
        auto parseViewIdFromFrame = [&]() -> uint64_t {
            constexpr std::string_view kPrefix = "prisma-view-";
            if (frameName.size() <= kPrefix.size() || frameName.compare(0, kPrefix.size(), kPrefix) != 0) {
                return 0;
            }
            uint64_t value = 0;
            for (size_t i = kPrefix.size(); i < frameName.size(); ++i) {
                const char c = frameName[i];
                if (c < '0' || c > '9') return 0;
                value = value * 10U + static_cast<uint64_t>(c - '0');
            }
            return value;
        };

        if (messageName == Messages::kInvokeResult) {
            if (payload.size() < 3) {
                logger::error("OnRendererMessage: malformed invokeResult payload (size {}).", payload.size());
                return true;
            }
            uint64_t requestId = 0;
            try { requestId = std::stoull(payload[0]); }
            catch (...) {
                logger::error("OnRendererMessage: invokeResult bad requestId '{}'.", payload[0]);
                return true;
            }
            Impl::InvokeEntry entry;
            bool have = false;
            {
                std::lock_guard lock(impl_->invokeMutex);
                auto it = impl_->pendingInvokes.find(requestId);
                if (it != impl_->pendingInvokes.end()) {
                    entry = std::move(it->second);
                    impl_->pendingInvokes.erase(it);
                    have = true;
                }
            }
            if (!have) {
                logger::warn("OnRendererMessage: invokeResult for unknown requestId {}.", requestId);
                return true;
            }
            if (entry.callback) {
                entry.callback(payload[2]);
            }
            return true;
        }

        if (messageName == Messages::kListenerInvoke) {
            if (payload.size() < 3) {
                logger::error("OnRendererMessage: malformed listenerInvoke payload (size {}).", payload.size());
                return true;
            }
            uint64_t viewId = 0;
            try { viewId = std::stoull(payload[0]); }
            catch (...) {
                logger::error("OnRendererMessage: listenerInvoke bad viewId '{}'.", payload[0]);
                return true;
            }
            if (parseViewIdFromFrame() != viewId) {
                logger::warn("OnRendererMessage: listenerInvoke viewId {} disagrees with frame '{}' — refusing.",
                             viewId, frameName);
                return true;
            }
            Communication::DispatchListenerInvoke(viewId, payload[1], payload[2]);
            return true;
        }

        if (messageName == Messages::kConsoleMessage) {
            if (payload.size() < 3) {
                logger::error("OnRendererMessage: malformed consoleMessage payload (size {}).", payload.size());
                return true;
            }
            uint64_t viewId = 0;
            try { viewId = std::stoull(payload[0]); }
            catch (...) {
                logger::error("OnRendererMessage: consoleMessage bad viewId '{}'.", payload[0]);
                return true;
            }
            if (parseViewIdFromFrame() != viewId) {
                logger::warn("OnRendererMessage: consoleMessage viewId {} disagrees with frame '{}' — refusing.",
                             viewId, frameName);
                return true;
            }
            Communication::DispatchConsoleMessage(viewId, payload[1], payload[2]);
            return true;
        }

        if (messageName == Messages::kDomReady) {
            if (payload.size() < 1) {
                logger::error("OnRendererMessage: malformed domReady payload (size {}).", payload.size());
                return true;
            }
            uint64_t viewId = 0;
            try { viewId = std::stoull(payload[0]); }
            catch (...) {
                logger::error("OnRendererMessage: domReady bad viewId '{}'.", payload[0]);
                return true;
            }
            if (parseViewIdFromFrame() != viewId) {
                logger::warn("OnRendererMessage: domReady viewId {} disagrees with frame '{}' — refusing.",
                             viewId, frameName);
                return true;
            }
            Communication::DispatchDomReady(viewId);
            return true;
        }

        return false;
    }
}
