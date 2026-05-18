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
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Cef/CefOsrClient.h"
#include "Cef/PrismaCefApp.h"
#include "Utils/DllLoader.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_task.h"

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
}
