#include "API.h"
#include "Cef/Browser/CefRuntime.h"
#include "Utils/Encoding.h"
#include "PrismaUI/ViewManager.h"
#include "PrismaUI/Communication.h"

PrismaView PluginAPI::PrismaUIInterface::CreateView(const char* htmlPath, PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback) noexcept
{
    return CreateViewInternal(htmlPath, onDomReadyCallback);
}

void PluginAPI::PrismaUIInterface::Invoke(PrismaView view, const char* script, PRISMA_UI_API::JSCallback callback) noexcept
{
    InvokeInternal(view, script, callback);
}

void PluginAPI::PrismaUIInterface::InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept
{
    if (!view || !functionName || !argument) {
        return;
    }

    std::string processedArgument;

    if (isValidUTF8(argument)) {
        processedArgument = argument;
    }
    else {
        processedArgument = convertFromANSIToUTF8(argument);
        if (processedArgument.empty()) {
            return;  // Conversion failed, cannot safely call
        }
    }

    return PrismaUI::Communication::InteropCall(view, functionName, processedArgument);
}

void PluginAPI::PrismaUIInterface::RegisterJSListener(PrismaView view, const char* fnName, PRISMA_UI_API::JSListenerCallback callback) noexcept
{
    RegisterJSListenerInternal(view, fnName, callback);
}

bool PluginAPI::PrismaUIInterface::HasFocus(PrismaView view) noexcept
{
    if (!view) {
        return false;
    }
    return PrismaUI::ViewManager::HasFocus(view);
}

bool PluginAPI::PrismaUIInterface::Focus(PrismaView view, bool pauseGame, bool disableFocusMenu) noexcept
{
    if (!view) {
        return false;
    }
    return PrismaUI::ViewManager::Focus(view, pauseGame, disableFocusMenu);
}

void PluginAPI::PrismaUIInterface::Unfocus(PrismaView view) noexcept
{
    if (!view) {
        return;
    }
    return PrismaUI::ViewManager::Unfocus(view);
}

void PluginAPI::PrismaUIInterface::Show(PrismaView view) noexcept
{
	if (!view) {
		return;
	}
	return PrismaUI::ViewManager::Show(view);
}

void PluginAPI::PrismaUIInterface::Hide(PrismaView view) noexcept
{
	if (!view) {
		return;
	}
	return PrismaUI::ViewManager::Hide(view);
}

bool PluginAPI::PrismaUIInterface::IsHidden(PrismaView view) noexcept
{
	if (!view) {
		return true;
	}
	return PrismaUI::ViewManager::IsHidden(view);
}

int PluginAPI::PrismaUIInterface::GetScrollingPixelSize(PrismaView view) noexcept
{
    if (!view) {
        return 0;
    }
    return PrismaUI::ViewManager::GetScrollingPixelSize(view);
}

void PluginAPI::PrismaUIInterface::SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept
{
    if (!view) {
        return;
    }
    return PrismaUI::ViewManager::SetScrollingPixelSize(view, pixelSize);
}

bool PluginAPI::PrismaUIInterface::IsValid(PrismaView view) noexcept
{
    if (!view) {
        return false;
    }
    return PrismaUI::ViewManager::IsValid(view);
}

void PluginAPI::PrismaUIInterface::Destroy(PrismaView view) noexcept
{
    if (!view) {
        return;
    }
    return PrismaUI::ViewManager::Destroy(view);
}

void PluginAPI::PrismaUIInterface::SetOrder(PrismaView view, int order) noexcept
{
	if (!view) {
		return;
	}
	return PrismaUI::ViewManager::SetOrder(view, order);
}

int PluginAPI::PrismaUIInterface::GetOrder(PrismaView view) noexcept
{
	if (!view) {
		return -1;
	}
	return PrismaUI::ViewManager::GetOrder(view);
}

void PluginAPI::PrismaUIInterface::CreateInspectorView(PrismaView view) noexcept
{
}

void PluginAPI::PrismaUIInterface::SetInspectorVisibility(PrismaView view, bool visible) noexcept
{
}

bool PluginAPI::PrismaUIInterface::IsInspectorVisible(PrismaView view) noexcept
{
    return false;
}

void PluginAPI::PrismaUIInterface::SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY, unsigned width,
    unsigned height) noexcept
{
}

bool PluginAPI::PrismaUIInterface::HasAnyActiveFocus() noexcept
{
	return PrismaUI::ViewManager::HasAnyActiveFocus();
}

void PluginAPI::PrismaUIInterface::RegisterConsoleCallback(PrismaView view, PRISMA_UI_API::ConsoleMessageCallback callback) noexcept
{
	if (!view) {
		return;
	}

	if (callback) {
		auto wrappedCallback = [callback](PrismaUI::Core::PrismaViewId id, PRISMA_UI_API::ConsoleMessageLevel level, const std::string& msg) {
			SKSE::GetTaskInterface()->AddTask([callback, id, level, msg]() {
				callback(id, level, msg.c_str());
			});
		};
		PrismaUI::ViewManager::RegisterConsoleCallback(view, wrappedCallback);
	} else {
		PrismaUI::ViewManager::RegisterConsoleCallback(view, nullptr);
	}
}

PrismaView PluginAPI::PrismaUIInterface::CreateViewV2(
    const char* htmlPath, PRISMA_UI_API::OnDomReadyCallbackWithState onDomReadyCallback, void* callbackState) noexcept
{
    auto callback = onDomReadyCallback
        ? std::function<void(PrismaUI::Core::PrismaViewId)>([onDomReadyCallback, callbackState](auto id) { onDomReadyCallback(id, callbackState); })
        : nullptr;

    return CreateViewInternal(htmlPath, callback);
}

void PluginAPI::PrismaUIInterface::InvokeV2(PrismaView view, const char* script,
    PRISMA_UI_API::JSCallbackWithState callback, void* callbackState) noexcept
{
    auto callbackFunc = callback
        ? std::function<void(const char*)>([callback, callbackState](auto id) { callback(id, callbackState); })
        : nullptr;

    InvokeInternal(view, script, callbackFunc);
}

void PluginAPI::PrismaUIInterface::RegisterJSListenerV2(PrismaView view, const char* functionName,
    PRISMA_UI_API::JSListenerCallbackWithState callback, void* callbackState) noexcept
{
    if (!callback) {
        return;
    }

    RegisterJSListenerInternal(view, functionName, [callback, callbackState](auto argument) { callback(argument, callbackState); });
}

void PluginAPI::PrismaUIInterface::RegisterConsoleCallbackV2(
    PrismaView view, PRISMA_UI_API::ConsoleMessageCallbackWithState callback, void* callbackState) noexcept
{
    if (!view || !callback) {
        return;
    }

    auto wrappedCallback = [callback, callbackState](PrismaUI::Core::PrismaViewId id, PRISMA_UI_API::ConsoleMessageLevel level, const std::string& msg) {
        SKSE::GetTaskInterface()->AddTask([callback, id, level, msg, callbackState] {
            callback(id, level, msg.c_str(), callbackState);
        });
    };
    PrismaUI::ViewManager::RegisterConsoleCallback(view, wrappedCallback);
}

void PluginAPI::PrismaUIInterface::OpenDevTools() noexcept
{
    PrismaUI::Cef::CefRuntime::GetSingleton().OpenDevTools();
}

void PluginAPI::PrismaUIInterface::CloseDevTools() noexcept
{
    PrismaUI::Cef::CefRuntime::GetSingleton().CloseDevTools();
}

bool PluginAPI::PrismaUIInterface::IsDevToolsOpen() noexcept
{
    return PrismaUI::Cef::CefRuntime::GetSingleton().IsDevToolsOpen();
}

PrismaView PluginAPI::PrismaUIInterface::CreateViewInternal(
    const char* htmlPath, std::function<void(PrismaUI::Core::PrismaViewId)> onDomReadyCallback) noexcept
{
    if (!htmlPath) {
        return 0;
    }

    std::move_only_function<void(PrismaUI::Core::PrismaViewId)> domReadyWrapper = nullptr;
    if (onDomReadyCallback) {
        domReadyWrapper = [callback = std::move(onDomReadyCallback)](PrismaUI::Core::PrismaViewId viewId) {
            SKSE::GetTaskInterface()->AddTask([callback = std::move(callback), id = viewId] {
                std::invoke(callback, id);
            });
        };
    }

    return PrismaUI::ViewManager::Create(htmlPath, std::move(domReadyWrapper));
}

void PluginAPI::PrismaUIInterface::InvokeInternal(PrismaView view, const char* script,
    std::function<void(const char*)> callback) noexcept
{
    if (!view || !script) {
        return;
    }

    std::string processedScript;

    if (isValidUTF8(script)) {
        processedScript = script;
    }
    else {
        processedScript = convertFromANSIToUTF8(script);
        if (processedScript.empty()) {
            return;  // Conversion failed, cannot safely invoke
        }
    }

    std::move_only_function<void(std::string)> callbackWrapper = nullptr;

    if (callback) {
        callbackWrapper = [callback = std::move(callback)](const std::string& result) {
            SKSE::GetTaskInterface()->AddTask([targetCallback = std::move(callback), data = result] {
                targetCallback(data.c_str());
            });
        };
    }

    return PrismaUI::Communication::Invoke(view, std::move(processedScript), std::move(callbackWrapper));
}

void PluginAPI::PrismaUIInterface::RegisterJSListenerInternal(PrismaView view, const char* functionName,
    std::function<void(const char*)> callback) noexcept
{
    if (!view || !functionName || !callback) {
        return;
    }

    PrismaUI::Core::SimpleJSCallback callbackWrapper = [callback = std::move(callback)](const std::string& arg) {
        SKSE::GetTaskInterface()->AddTask([targetCallback = std::move(callback), data = arg] {
            targetCallback(data.c_str());
        });
    };

    return PrismaUI::Communication::RegisterJSListener(view, functionName, callbackWrapper);
}
