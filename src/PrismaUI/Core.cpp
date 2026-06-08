#include "Core.h"

#include <DirectXTK/WICTextureLoader.h>

#include <vector>

#include "Cef/Browser/CefRuntime.h"
#include "Communication.h"
#include "InputHandler.h"
#include "Utils/DllLoader.h"
#include "ViewManager.h"
#include "ViewOperationQueue.h"
#include "ViewRenderer.h"

namespace PrismaUI::Core {
    using namespace PrismaUI::ViewRenderer;
    using namespace PrismaUI::ViewManager;
    using namespace PrismaUI::InputHandler;

    NanoIdGenerator generator;
    std::atomic<bool> coreInitialized = false;
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

    PrismaView::~PrismaView() = default;

    void InitializeCoreSystem() {
        logger::info("Initializing PrismaUI Core System...");
        InitHooks();

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
                Initialize(hWnd, &views, &viewsMutex);

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

        if (!coreInitialized) return;

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

        // Process pending operations and queued input for all views.
        ViewOperationQueue::ProcessAllViewOperations();
        ProcessEvents();

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
        coreInitialized = false;
        logger::info("PrismaUI Core System shut down complete.");
    }
}  // namespace PrismaUI::Core
