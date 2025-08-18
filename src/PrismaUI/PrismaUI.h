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

typedef uint64_t PrismaViewId;

namespace PrismaUI {
	using namespace ultralight;

	namespace Core {
		class MyLoadListener;
		class MyViewListener;

		struct PrismaView {
			PrismaViewId id;
			RefPtr<View> ultralightView = nullptr;
			std::string htmlPathToLoad;
			std::atomic<bool> isHidden = false;
			std::unique_ptr<MyLoadListener> loadListener;
			std::unique_ptr<MyViewListener> viewListener;
			std::atomic<bool> isLoadingFinished = false;
			std::function<void(const PrismaViewId&)> domReadyCallback;
			int scrollingPixelSize = 28;
			std::atomic<bool> isPaused = false;

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

		extern SingleThreadExecutor uiThread;
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

		PrismaViewId Create(const std::string& htmlPath, std::function<void(PrismaViewId)> onDomReadyCallback = nullptr);
		void Shutdown();
		void Show(const PrismaViewId& viewId);
		void Hide(const PrismaViewId& viewId);
		bool IsHidden(const PrismaViewId& viewId);
		bool Focus(const PrismaViewId& viewId, bool pauseGame = false);
		void Unfocus(const PrismaViewId& viewId);
		bool HasFocus(const PrismaViewId& viewId);
		bool ViewHasInputFocus(const PrismaViewId& viewId);
		void RegisterJSListener(const PrismaViewId& viewId, const std::string& name, SimpleJSCallback callback);
		JSValueRef JSCallbackDispatcher(JSContextRef ctx, JSObjectRef function,
			JSObjectRef thisObject, size_t argumentCount,
			const JSValueRef arguments[], JSValueRef* exception);
		void BindJSCallbacks(const PrismaViewId& viewId);
		void Invoke(const PrismaViewId& viewId, const String& script, std::function<void(std::string)> callback = nullptr);
		void InteropCall(const PrismaViewId& viewId, const std::string& functionName, const std::string& argument);
		void Destroy(const PrismaViewId& viewId);
		bool IsValid(const PrismaViewId& viewId);
		void InitHooks();
		void InitGraphics();
		void InitializeCoreSystem();
		void UpdateLogic();
		void RenderViews();
		void RenderSingleView(std::shared_ptr<PrismaView> viewData);
		void CopyBitmapToBuffer(std::shared_ptr<PrismaView> viewData);
		void DrawViews();
		void UpdateSingleTextureFromBuffer(std::shared_ptr<PrismaView> viewData);
		void CopyPixelsToTexture(PrismaView* viewData, void* pixels, uint32_t width, uint32_t height, uint32_t stride);
		void DrawSingleTexture(std::shared_ptr<PrismaView> viewData);
		void DrawCursor();
		void ReleaseViewTexture(PrismaView* viewData);
		void D3DPresent(uint32_t a_p1);
		void SetScrollingPixelSize(const PrismaViewId& viewId, int pixelSize);
		int GetScrollingPixelSize(const PrismaViewId& viewId);
	}

	namespace InputHandler {
		using InputEvent = std::variant<
			MouseEvent,
			ScrollEvent,
			KeyEvent
		>;

		void Initialize(HWND gameHwnd, SingleThreadExecutor* coreExecutor, std::map<PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap, std::shared_mutex* viewsMapMutex);
		void SetOriginalWndProc(WNDPROC originalProc);

		void EnableInputCapture(const PrismaViewId& viewId);
		void DisableInputCapture(const PrismaViewId& viewId);

		bool IsInputCaptureActiveForView(const PrismaViewId& viewId);

		bool IsAnyInputCaptureActive();

		LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
		void ProcessEvents();
		void Shutdown();
	}

	namespace SkyrimKeyHandler {
		int ConvertSkyrimDXScanCodeToUltralight(uint32_t skyrimDXScanCode);
	}

	namespace WindowsKeyHandler {
		int WinKeyToUltralightKey(UINT win_key);
		std::string GetUltralightKeyIdentifier(int ul_key);
		void GetUltralightModifiers(ultralight::KeyEvent& ev);
		ultralight::KeyEvent CreateKeyEvent(ultralight::KeyEvent::Type type, WPARAM wParam, LPARAM lParam);
	}
}
