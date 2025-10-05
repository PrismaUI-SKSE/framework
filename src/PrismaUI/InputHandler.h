#pragma once

#include <Ultralight/Ultralight.h>
#include <Utils/WinKeyHandler/WinKeyHandler.h>

#include <map>
#include <memory>
#include <shared_mutex>
#include <variant>

class SingleThreadExecutor;

namespace PrismaUI::Core {
	typedef uint64_t PrismaViewId;
	struct PrismaView;
}

namespace PrismaUI::InputHandler {
	using namespace ultralight;

	using InputEvent = std::variant<
		MouseEvent,
		ScrollEvent,
		KeyEvent
	>;

	void Initialize(HWND gameHwnd, SingleThreadExecutor* coreExecutor, std::map<Core::PrismaViewId, std::shared_ptr<Core::PrismaView>>* viewsMap, std::shared_mutex* viewsMapMutex);
	void SetOriginalWndProc(WNDPROC originalProc);

	void EnableInputCapture(const Core::PrismaViewId& viewId);
	void DisableInputCapture(const Core::PrismaViewId& viewId);

	bool IsInputCaptureActiveForView(const Core::PrismaViewId& viewId);

	bool IsAnyInputCaptureActive();

	LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	void ProcessEvents();
	void Shutdown();
}
