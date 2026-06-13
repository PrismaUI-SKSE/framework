#pragma once

#include <DirectXTK/CommonStates.h>
#include <DirectXTK/SpriteBatch.h>
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>

#include "Hooks/Hooks.h"
#include "Menus/FocusMenu/FocusMenu.h"

namespace PRISMA_UI_API {
    enum class ConsoleMessageLevel : uint8_t;
}

namespace PrismaUI::Core {

    typedef uint64_t PrismaViewId;

    struct PrismaView {
        PrismaViewId id;
        // CEF shell iframe name and resolved URL.
        std::string iframeName;
        std::string resolvedUrl;
        std::string originalUrl;
        // Lifecycle/state atomics owned by the native CEF shell adapter.
        std::atomic<bool> isHidden = false;
        std::atomic<bool> isFocused = false;
        std::atomic<bool> iframeCreateRequested = false;
        std::atomic<bool> iframeReady = false;  // Set by CEF shell load callbacks (wired in Step 5/7).
        std::atomic<bool> destroyRequested = false;
        std::atomic<bool> isPaused = false;
        int scrollingPixelSize = 28;
        int order = 0;
        std::function<void(PrismaViewId)> domReadyCallback;
        std::function<void(PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)>
            consoleMessageCallback;

        // Operation queue fields for thread-safe sequential execution
        std::mutex operationMutex;
        std::queue<std::function<void()>> pendingOperations;
        std::atomic<bool> isProcessingOperation = false;
        std::atomic<int> queuedOperationsCount = 0;

        ~PrismaView();
    };

    extern std::atomic_uint64_t nextViewId;
    extern std::atomic<bool> coreInitialized;
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
