#pragma once

#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#include <Ultralight/StringSTL.h>
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>

#include <Utils/NanoID.h>
#include <Utils/SingleThreadExecutor.h>
#include <Utils/RepeatingTaskRunner.h>
#include <Hooks/Hooks.h>
#include <Menus/FocusMenu/FocusMenu.h>

#include <d3d11.h>
#include <DirectXTK/SpriteBatch.h>
#include <DirectXTK/CommonStates.h>
#include <DirectXTK/WICTextureLoader.h>
#include <wrl/client.h>
#include <windows.h>
#include <memory>
#include <atomic>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <future>
#include <stdexcept>
#include <shared_mutex>
#include <windowsx.h>
#include <variant>
#include <cstdint>
#include <codecvt>

namespace PrismaUI::Listeners {
	class MyLoadListener;
	class MyViewListener;
}

namespace PrismaUI::Core {
	using namespace ultralight;

	typedef uint64_t PrismaViewId;

	struct PrismaView {
		PrismaViewId id;
		RefPtr<View> ultralightView = nullptr;
		std::string htmlPathToLoad;
		std::atomic<bool> isHidden = false;
		std::unique_ptr<Listeners::MyLoadListener> loadListener;
		std::unique_ptr<Listeners::MyViewListener> viewListener;
		std::atomic<bool> isLoadingFinished = false;
		std::function<void(const PrismaViewId&)> domReadyCallback;
		int scrollingPixelSize = 28;
		std::atomic<bool> isPaused = false;
		int order = 0;

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

		~PrismaView();
	};

	extern SingleThreadExecutor ultralightThread;
	extern std::unique_ptr<RepeatingTaskRunner> logicRunner;
	extern NanoIdGenerator generator;
	extern std::atomic<bool> coreInitialized;

	extern RefPtr<Renderer> renderer;
	extern ID3D11Device* d3dDevice;
	extern ID3D11DeviceContext* d3dContext;
	extern HWND hWnd;
	extern WNDPROC s_originalWndProc;
	extern RE::BSGraphics::ScreenSize screenSize;
	extern std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
	extern std::unique_ptr<DirectX::CommonStates> commonStates;
	extern Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorTexture;

	extern std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
	extern std::shared_mutex viewsMutex;

	using SimpleJSCallback = std::function<void(std::string)>;

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
