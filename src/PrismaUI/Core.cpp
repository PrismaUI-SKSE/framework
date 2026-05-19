#include "Core.h"

#include <eh.h>  // For _set_se_translator

#include "Cef/CefRuntime.h"
#include "Communication.h"
#include "InputHandler.h"
#include "Utils/DllLoader.h"
#include "ViewManager.h"
#include "ViewOperationQueue.h"
#include "ViewRenderer.h"

namespace {
    // SEH exception class to convert structured exceptions to C++ exceptions
    // Copies all relevant data from EXCEPTION_POINTERS since that pointer is only
    // valid during the translator call
    class SEHException : public std::exception {
    public:
        SEHException(unsigned int code, EXCEPTION_POINTERS* ep)
            : code_(code), address_(nullptr), accessType_(0), accessAddress_(0) {
            if (ep && ep->ExceptionRecord) {
                address_ = ep->ExceptionRecord->ExceptionAddress;
                // For access violations, capture the operation type and target address
                if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
                    accessType_ = ep->ExceptionRecord->ExceptionInformation[0];
                    accessAddress_ = ep->ExceptionRecord->ExceptionInformation[1];
                }
            }
        }

        const char* what() const noexcept override { return "Windows Structured Exception"; }

        unsigned int code() const { return code_; }
        void* address() const { return address_; }

        std::string details() const {
            std::string msg;
            switch (code_) {
                case EXCEPTION_ACCESS_VIOLATION:
                    msg = "Access Violation";
                    {
                        const char* op = accessType_ == 0 ? "read" : "write";
                        char buf[128];
                        snprintf(buf, sizeof(buf), " (%s at 0x%p)", op, (void*)accessAddress_);
                        msg += buf;
                    }
                    break;
                case EXCEPTION_STACK_OVERFLOW:
                    msg = "Stack Overflow";
                    break;
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                    msg = "Integer Divide by Zero";
                    break;
                default:
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Code 0x%08X", code_);
                    msg = buf;
                    break;
            }
            return msg;
        }

    private:
        unsigned int code_;
        void* address_;
        ULONG_PTR accessType_;     // 0 = read, 1 = write, 8 = DEP violation
        ULONG_PTR accessAddress_;  // Address that was accessed
    };

    void SEHTranslator(unsigned int code, EXCEPTION_POINTERS* ep) {
        // Stack overflow cannot be safely translated to a C++ exception because
        // exception handling requires stack space for unwinding, which we don't have.
        // Let it propagate as an SEH exception - the system will terminate the
        // process.
        if (code == EXCEPTION_STACK_OVERFLOW) {
            // Don't throw - just return and let the SEH continue
            // The process will likely terminate, but that's safer than undefined
            // behavior
            return;
        }
        throw SEHException(code, ep);
    }
}  // namespace

namespace PrismaUI::Core {
    using namespace PrismaUI::ViewRenderer;
    using namespace PrismaUI::ViewManager;
    using namespace PrismaUI::InputHandler;

    SingleThreadExecutor ultralightThread;
    NanoIdGenerator generator;
    std::atomic<bool> coreInitialized = false;
    std::atomic<bool> rendererInitFailed = false;

    RefPtr<Renderer> renderer;
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    HWND hWnd = nullptr;

    RE::BSGraphics::ScreenSize screenSize;

    std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
    std::unique_ptr<DirectX::CommonStates> commonStates;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorTexture;

    std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
    std::shared_mutex viewsMutex;

    std::map<std::pair<PrismaViewId, std::string>, JSCallbackData> PrismaUI::Core::jsCallbacks;
    std::mutex PrismaUI::Core::jsCallbacksMutex;

    inline REL::Relocation<Hooks::D3DPresentHook::D3DPresentFunc> RealD3dPresentFunc;

    PrismaView::~PrismaView() { ViewRenderer::ReleaseViewTexture(this); }

    void InitializeCoreSystem() {
        logger::info("Initializing PrismaUI Core System...");
        InitHooks();

        const auto basePath = Utils::GetBasePath();
        ultralightThread
            .submit([basePath]() {
                try {
                    Platform& plat = Platform::instance();
                    plat.set_font_loader(ultralight::GetPlatformFontLoader());

                    plat.set_file_system(ultralight::GetPlatformFileSystem(basePath.string().c_str()));

                    Config config;
                    config.resource_path_prefix = "resources/";
                    plat.set_config(config);

                    renderer = Renderer::Create();
                    if (!renderer) {
                        logger::critical("Failed to create Ultralight Renderer!");
                        rendererInitFailed = true;
                    } else {
                        logger::info(
                            "Ultralight Platform configured and Renderer created on UI "
                            "thread.");
                    }
                } catch (const std::exception& e) {
                    logger::critical(
                        "Exception during Ultralight Platform/Renderer init on UI "
                        "thread: {}",
                        e.what());
                    rendererInitFailed = true;
                } catch (...) {
                    logger::critical(
                        "Unknown exception during Ultralight Platform/Renderer init on "
                        "UI thread.");
                    rendererInitFailed = true;
                }
            })
            .get();

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
            logger::critical("InitGraphics: RenderManager is null!");
            return;
        }
        auto runtimeData = renderManager->GetRuntimeData();
        if (!d3dDevice) d3dDevice = reinterpret_cast<ID3D11Device*>(runtimeData.forwarder);
        if (!d3dContext) d3dContext = reinterpret_cast<ID3D11DeviceContext*>(runtimeData.context);

        if (!hWnd && runtimeData.renderWindows && runtimeData.renderWindows->hWnd) {
            hWnd = reinterpret_cast<HWND>(runtimeData.renderWindows->hWnd);
            screenSize = renderManager->GetScreenSize();

            static std::atomic<bool> input_handler_initialized = false;
            bool expected_ih_init = false;

            if (input_handler_initialized.compare_exchange_strong(expected_ih_init, true)) {
                Initialize(hWnd, &ultralightThread, &views, &viewsMutex);

                // Thread-affinity issue with SetWindowSubclass:
                // - Windows requires SetWindowSubclass to be called from the window's owning thread
                // - The game HWND is created on the main thread (thread that creates the window)
                // - We are currently on the render thread (D3D Present hook)
                // - Behavior varies across systems:
                //   * Some systems: SetWindowSubclass works cross-thread (Windows 10+)
                //   * Other systems: SetWindowSubclass fails unless called from main thread
                // Solution: Try direct installation first (faster), fallback to main thread if it fails

                logger::info("Attempting to install WndProc hook from render thread...");
                if (InstallWndProcHook()) {
                    logger::info("WndProc hook installed successfully from render thread.");
                } else {
                    logger::warn("Direct installation failed, scheduling on main thread...");
                    SKSE::GetTaskInterface()->AddTask([]() {
                        logger::info("Attempting to install WndProc hook from main thread...");
                        if (InstallWndProcHook()) {
                            logger::info("WndProc hook installed successfully from main thread.");
                        } else {
                            logger::error("Failed to install WndProc hook even from main thread!");
                        }
                    });
                }
            }
        } else if (!hWnd) {
            logger::warn("InitGraphics: Could not obtain HWND.");
        }

        if (d3dDevice && d3dContext) {
            if (!commonStates || !spriteBatch) {
                try {
                    commonStates = std::make_unique<DirectX::CommonStates>(d3dDevice);
                    spriteBatch = std::make_unique<DirectX::SpriteBatch>(d3dContext);
                    logger::info("DirectXTK SpriteBatch and CommonStates (re)initialized.");
                } catch (const std::exception& e) {
                    logger::critical("Failed to initialize DirectXTK: {}", e.what());
                    commonStates.reset();
                    spriteBatch.reset();
                }
            }

            if (!cursorTexture && d3dDevice) {
                auto cursorPath = Utils::GetBasePath() / "misc" / "cursor.png";
                HRESULT hr =
                    DirectX::CreateWICTextureFromFile(d3dDevice, cursorPath.wstring().c_str(), nullptr, &cursorTexture);
                if (SUCCEEDED(hr)) {
                    logger::info("Cursor texture loaded successfully.");
                } else {
                    logger::error("Failed to load cursor texture from '{}'. HRESULT: 0x{:08X}", cursorPath.string(),
                                  static_cast<unsigned int>(hr));
                    cursorTexture.Reset();
                }
            }

            if (hWnd && d3dDevice && d3dContext && screenSize.width != 0 && screenSize.height != 0) {
                Cef::CefRuntime::GetSingleton().Initialize(hWnd, d3dDevice, d3dContext, screenSize.width,
                                                           screenSize.height);
            }
        } else {
            logger::error("Cannot initialize DirectXTK: D3D device or context is null.");
            commonStates.reset();
            spriteBatch.reset();
        }
    }

    void D3DPresent(uint32_t a_p1) {
        RealD3dPresentFunc(a_p1);

        if (!coreInitialized || rendererInitFailed) return;

        if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0 ||
            screenSize.height == 0) {
            InitGraphics();
            if (!d3dDevice || !d3dContext || !spriteBatch || !commonStates || !hWnd || screenSize.width == 0 ||
                screenSize.height == 0)
                return;
        }

        if (auto* renderManager = RE::BSGraphics::Renderer::GetSingleton()) {
            const auto currentScreenSize = renderManager->GetScreenSize();
            if (currentScreenSize.width != 0 && currentScreenSize.height != 0 &&
                (currentScreenSize.width != screenSize.width || currentScreenSize.height != screenSize.height)) {
                screenSize = currentScreenSize;
                Cef::CefRuntime::GetSingleton().Resize(screenSize.width, screenSize.height);
            }
        }

        Cef::CefRuntime::GetSingleton().BeginFrame();
        Cef::CefRuntime::GetSingleton().UpdateOverlayTexture(d3dDevice, d3dContext);

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
                logger::debug(
                    "D3DPresent: Releasing D3D resources for View [{}] from render "
                    "thread",
                    viewId);
                ViewRenderer::ReleaseViewTexture(viewData.get());
                viewData->pendingResourceRelease = false;
            }
        }

        // Process pending operations for all views
        ViewOperationQueue::ProcessAllViewOperations();

        auto ultralightFuture = ultralightThread.submit([dev = d3dDevice, ctx = d3dContext, hwnd = hWnd]() {
            // Enable SEH to C++ exception translation for this thread (only needs to be
            // set once per thread)
            static bool sehTranslatorSet = false;
            if (!sehTranslatorSet) {
                _set_se_translator(SEHTranslator);
                sehTranslatorSet = true;
            }

            try {
                if (!dev || !ctx || !hwnd) {
                    logger::warn("UI Thread: D3D device/context/hwnd is null, skipping frame.");
                    return;
                }

                // Capture renderer locally to avoid race with shutdown
                auto localRenderer = renderer;
                if (!localRenderer) {
                    logger::warn("UI Thread: Renderer is null, skipping frame.");
                    return;
                }

                // Recovery and view-creation loops moved to native CEF path (Step 6).
                // Ultralight per-view creation / LoadURL recovery is retired here; CefRuntime
                // owns iframe lifecycle and replays createView calls when the shell becomes ready.

                ProcessEvents();

                if (localRenderer) {
                    localRenderer->Update();
                    localRenderer->RefreshDisplay(0);
                    localRenderer->Render();
                }

                RenderViews();
            } catch (const SEHException& seh) {
                logger::critical("UI Thread: SEH Exception in render loop: {} at address 0x{:p}", seh.details(),
                                 seh.address());
            } catch (const std::exception& e) {
                logger::critical("UI Thread: Exception in render loop: {}", e.what());
            } catch (...) {
                logger::critical(
                    "UI Thread: Unknown exception in render loop (likely Ultralight "
                    "internal error)");
            }
        });

        // Wait for UI thread but handle any exceptions that might have escaped
        try {
            ultralightFuture.get();
        } catch (const std::exception& e) {
            logger::error("D3DPresent: Exception from UI thread: {}", e.what());
        } catch (...) {
            logger::error("D3DPresent: Unknown exception from UI thread");
        }

        // Per-view texture upload loop retired in Step 6 — CEF overlay is the single drawn surface.

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
                if (pair.second) {
                    // Mark each view as destroyRequested so any in-flight queue entries
                    // observe it and no-op before reaching CEF or render state (Step 6).
                    pair.second->destroyRequested.store(true, std::memory_order_release);
                }
            }
        }

        for (const auto& id : viewIdsToDestroy) {
            try {
                ViewManager::Destroy(id);
            } catch (const std::exception& e) {
                logger::error("Error destroying view [{}] during shutdown: {}", id, e.what());
            }
        }


        Cef::CefRuntime::GetSingleton().ReleaseRenderResources();
        Cef::CefRuntime::GetSingleton().Shutdown();
        cursorTexture.Reset();
        spriteBatch.reset();
        commonStates.reset();
        logger::debug("DirectXTK resources released.");

        InputHandler::Shutdown();

        d3dDevice = nullptr;
        d3dContext = nullptr;
        hWnd = nullptr;

        {
            std::unique_lock lock(viewsMutex);
            views.clear();
        }
        if (renderer) {
            // Move renderer to the lambda so it's the sole owner,
            // ensuring release happens on the UI thread
            ultralightThread
                .submit([renderer_moved = std::move(renderer)]() mutable {
                    logger::info("Releasing global renderer on UI thread.");
                    renderer_moved = nullptr;
                })
                .get();
        }


        coreInitialized = false;
        logger::info("PrismaUI Core System shut down complete.");
    }
}  // namespace PrismaUI::Core
