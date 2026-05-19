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

namespace PrismaUI::Listeners {
    class MyLoadListener;
    class MyViewListener;
}

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

        // ---- Transitional Ultralight fields (Step 6) ----
        // Declared so out-of-scope modules (Communication, InputHandler, ImeHelper, Inspector,
        // Listeners, ViewRenderer) keep compiling. ViewManager/Core no longer assign or read them.
        // Steps 7-9 retire those dependents; Step 10 deletes the fields.
        RefPtr<View> ultralightView = nullptr;
        RefPtr<View> inspectorView = nullptr;
        std::string htmlPathToLoad;                                  // deprecated; superseded by resolvedUrl + iframeCreateRequested
        std::string originalUrl;                                     // retained for Step 11 recovery policy
        std::string lastLoadedUrl;                                   // deprecated; removed in Step 10
        std::unique_ptr<Listeners::MyLoadListener> loadListener;     // deprecated; removed in Step 10
        std::unique_ptr<Listeners::MyViewListener> viewListener;     // deprecated; removed in Step 10
        std::atomic<bool> isLoadingFinished = false;
        std::atomic<bool> inspectorVisible = false;                  // deprecated; Step 9 replaces with DevTools API
        std::atomic<bool> needsRecovery = false;                     // deprecated; Step 11 reintroduces recovery
        std::atomic<int> recoveryAttempts = 0;                       // deprecated; Step 11 reintroduces recovery

        // Inspector rendering data
        std::vector<std::byte> inspectorPixelBuffer;
        uint32_t inspectorBufferWidth = 0;
        uint32_t inspectorBufferHeight = 0;
        uint32_t inspectorBufferStride = 0;
        std::mutex inspectorBufferMutex;
        std::atomic<bool> inspectorFrameReady = false;
        std::atomic<bool> inspectorPointerHover = false;
        ID3D11Texture2D* inspectorTexture = nullptr;
        ID3D11ShaderResourceView* inspectorTextureView = nullptr;
        uint32_t inspectorTextureWidth = 0;
        uint32_t inspectorTextureHeight = 0;
        float inspectorPosX = 0.0f;
        float inspectorPosY = 0.0f;
        uint32_t inspectorDisplayWidth = 0;
        uint32_t inspectorDisplayHeight = 0;
        float inspectorOpacity = 1.0f;

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

    // Inspector View functions
    void CreateInspectorView(const PrismaViewId& viewId);
    void SetInspectorVisibility(const PrismaViewId& viewId, bool visible);
    bool IsInspectorVisible(const PrismaViewId& viewId);
    void SetInspectorBounds(const PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width,
                            uint32_t height);
}
