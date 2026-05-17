#include "PCH.h"

#ifdef GetNextSibling
#    undef GetNextSibling
#endif

#include "Cef/CefRuntime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

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
