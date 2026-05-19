#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/StringSTL.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

#include <DirectXTK/CommonStates.h>
#include <DirectXTK/SpriteBatch.h>
#include <DirectXTK/WICTextureLoader.h>
#include <d3d11.h>
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "Hooks/Hooks.h"
#include "Menus/FocusMenu/FocusMenu.h"
#include "Utils/NanoID.h"
#include "Utils/SingleThreadExecutor.h"

namespace PRISMA_UI_API {
    enum class ConsoleMessageLevel : uint8_t;
}

// Listeners namespace deleted in Step 7. The Ultralight LoadListener/ViewListener
// shims used to live here; their responsibilities now flow through CefRuntime +
// PrismaCefRenderApp + Communication::Dispatch* (see Step 7 plan).

namespace PrismaUI::Core {
    using namespace ultralight;

    typedef uint64_t PrismaViewId;

    struct PrismaView {
        PrismaViewId id;
        // CEF shell iframe naming and resolved URL (native source of truth for CEF rewire — Step 6).
        std::string iframeName;
        std::string resolvedUrl;
        // Lifecycle/state atomics (native — no Ultralight involvement).
        std::atomic<bool> isHidden = false;
        std::atomic<bool> isFocused = false;
        std::atomic<bool> iframeCreateRequested = false;
        std::atomic<bool> iframeReady = false;    // Set by CEF shell load callbacks (wired in Step 5/7).
        std::atomic<bool> destroyRequested = false;
        std::atomic<bool> isPaused = false;
        int scrollingPixelSize = 28;
        int order = 0;
        std::move_only_function<void(const PrismaViewId&)> domReadyCallback;
        std::move_only_function<void(PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> consoleMessageCallback;

        // ---- Transitional Ultralight fields (Step 6/7) ----
        // Declared so out-of-scope modules (InputHandler, ImeHelper, ViewRenderer)
        // keep compiling. ViewManager/Core/Communication no longer touch these.
        // Steps 8-10 retire those dependents.
        RefPtr<View> ultralightView = nullptr;
        std::string htmlPathToLoad;                                  // deprecated; superseded by resolvedUrl + iframeCreateRequested
        std::string originalUrl;                                     // retained for Step 11 recovery policy
        std::string lastLoadedUrl;                                   // deprecated; removed in Step 10
        std::atomic<bool> isLoadingFinished = false;                 // deprecated; ViewRenderer/InputHandler still read it (Step 10)
        std::atomic<bool> needsRecovery = false;                     // deprecated; Step 11 reintroduces recovery
        std::atomic<int> recoveryAttempts = 0;                       // deprecated; Step 11 reintroduces recovery

        // Primary view rendering data
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* textureView = nullptr;
        uint32_t textureWidth = 0;
        uint32_t textureHeight = 0;
        std::vector<std::byte> pixelBuffer;
        uint32_t bufferWidth = 0;
        uint32_t bufferHeight = 0;
        uint32_t bufferStride = 0;
        std::mutex bufferMutex;
        std::atomic<bool> newFrameReady = false;
        std::atomic<bool> pendingResourceRelease = false;

        // Operation queue fields for thread-safe sequential execution
        std::mutex operationMutex;
        std::queue<std::function<void()>> pendingOperations;
        std::atomic<bool> isProcessingOperation = false;
        std::atomic<int> queuedOperationsCount = 0;

        ~PrismaView();
    };

    extern SingleThreadExecutor ultralightThread;
    extern NanoIdGenerator generator;
    extern std::atomic<bool> coreInitialized;
    extern std::atomic<bool> rendererInitFailed;

    extern RefPtr<Renderer> renderer;
    extern ID3D11Device* d3dDevice;
    extern ID3D11DeviceContext* d3dContext;
    extern HWND hWnd;
    extern RE::BSGraphics::ScreenSize screenSize;
    extern std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
    extern std::unique_ptr<DirectX::CommonStates> commonStates;
    extern Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorTexture;

    extern std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
    extern std::shared_mutex viewsMutex;

    using SimpleJSCallback = std::function<void(const std::string&)>;

    struct JSCallbackData {
        PrismaViewId viewId;
        std::string name;
        SimpleJSCallback callback;
    };

    extern std::map<std::pair<PrismaViewId, std::string>, JSCallbackData> jsCallbacks;
    extern std::mutex jsCallbacksMutex;

    extern inline REL::Relocation<Hooks::D3DPresentHook::D3DPresentFunc> RealD3dPresentFunc;

    void InitializeCoreSystem();
    void InitHooks();
    void InitGraphics();
    void D3DPresent(uint32_t a_p1);
    void Shutdown();

}
