#include "API.h"
#include <Utils/Encoding.h>
#include <PrismaUI/ViewManager.h>
#include <PrismaUI/Communication.h>

PrismaView PluginAPI::PrismaUIInterface::CreateView(const char* htmlPath, PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback) noexcept
{
    if (!htmlPath) {
        return 0;
    }

    std::function<void(PrismaUI::Core::PrismaViewId)> domReadyWrapper = nullptr;
    if (onDomReadyCallback) {
        domReadyWrapper = [onDomReadyCallback](PrismaUI::Core::PrismaViewId viewId) {
            onDomReadyCallback(viewId);
        };
    }

    return PrismaUI::ViewManager::Create(htmlPath, domReadyWrapper);
}

void PluginAPI::PrismaUIInterface::Invoke(PrismaView view, const char* script, PRISMA_UI_API::JSCallback callback) noexcept
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
    }

    ultralight::String _script(processedScript.c_str());

    std::function<void(std::string)> callbackWrapper = nullptr;
    if (callback) {
        callbackWrapper = [callback](const std::string& result) {
            callback(result.c_str());
            };
    }
    
    return PrismaUI::Communication::Invoke(view, _script, callbackWrapper);
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
    }

    return PrismaUI::Communication::InteropCall(view, functionName, processedArgument);
}

void PluginAPI::PrismaUIInterface::RegisterJSListener(PrismaView view, const char* fnName, PRISMA_UI_API::JSListenerCallback callback) noexcept
{
    if (!view || !fnName || !callback) {
        return;
    }

    std::function<void(std::string)> callbackWrapper = [callback](const std::string& arg) {
        callback(arg.c_str());
        };

    return PrismaUI::Communication::RegisterJSListener(view, fnName, callbackWrapper);
}

bool PluginAPI::PrismaUIInterface::HasFocus(PrismaView view) noexcept
{
    if (!view) {
        return false;
    }
    return PrismaUI::ViewManager::HasFocus(view);
}

bool PluginAPI::PrismaUIInterface::Focus(PrismaView view, bool pauseGame) noexcept
{
    if (!view) {
        return false;
    }
    return PrismaUI::ViewManager::Focus(view, pauseGame);
}

void PluginAPI::PrismaUIInterface::Unfocus(PrismaView view) noexcept
{
    if (!view) {
        return;
    }
    return PrismaUI::ViewManager::Unfocus(view);
}

int PluginAPI::PrismaUIInterface::GetScrollingPixelSize(PrismaView view) noexcept
{
    if (!view) {
        return 28;
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
