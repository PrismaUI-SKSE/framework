#include "Communication.h"

#include "Cef/Browser/CefRuntime.h"
#include "Core.h"
#include "PrismaUI_API.h"
#include "ViewManager.h"

namespace PrismaUI::Communication {
    using namespace Core;

    namespace {
        PRISMA_UI_API::ConsoleMessageLevel ParseConsoleLevel(const std::string& level) {
            if (level == "warn" || level == "warning") return PRISMA_UI_API::ConsoleMessageLevel::Warning;
            if (level == "error") return PRISMA_UI_API::ConsoleMessageLevel::Error;
            if (level == "debug") return PRISMA_UI_API::ConsoleMessageLevel::Debug;
            if (level == "info") return PRISMA_UI_API::ConsoleMessageLevel::Info;
            return PRISMA_UI_API::ConsoleMessageLevel::Log;
        }
    }

    void Invoke(const Core::PrismaViewId& viewId, std::string script,
                std::move_only_function<void(std::string)> callback) {
        {
            std::shared_lock lock(viewsMutex);
            if (views.find(viewId) == views.end()) {
                logger::warn("Invoke: View ID [{}] not found.", viewId);
                if (callback) callback(std::string());
                return;
            }
        }

        // CefRuntime owns the in-flight Invoke registry and fires the callback exactly
        // once. Wrap the move_only_function in a std::function shim because
        // CefRuntime's surface uses copyable functions internally.
        std::function<void(std::string)> shim;
        if (callback) {
            auto shared = std::make_shared<std::move_only_function<void(std::string)>>(std::move(callback));
            shim = [shared](std::string result) {
                if (shared && *shared) {
                    (*shared)(std::move(result));
                    // Single-shot — clear to release any captured state.
                    *shared = nullptr;
                }
            };
        }

        Cef::CefRuntime::GetSingleton().InvokeScript(viewId, std::move(script), std::move(shim));
    }

    void RegisterJSListener(const Core::PrismaViewId& viewId, const std::string& name,
                            Core::SimpleJSCallback callback) {
        if (!ViewManager::IsValid(viewId)) {
            logger::error("RegisterJSListener: View ID [{}] not found.", viewId);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(jsCallbacksMutex);
            JSCallbackData data;
            data.viewId = viewId;
            data.name = name;
            data.callback = std::move(callback);
            jsCallbacks[std::make_pair(viewId, name)] = std::move(data);
            logger::info("RegisterJSListener: stored callback '{}' for view [{}]", name, viewId);
        }

        // Tell the renderer subprocess to install (or refresh) the window[name] trampoline.
        // If the iframe isn't attached yet, CefRuntime logs and the renderer will install
        // it lazily on the next OnContextCreated for that iframe.
        Cef::CefRuntime::GetSingleton().RegisterListener(viewId, name, /*unused*/ nullptr);
    }

    void InteropCall(const Core::PrismaViewId& viewId, const std::string& functionName,
                     const std::string& argument) {
        {
            std::shared_lock lock(viewsMutex);
            if (views.find(viewId) == views.end()) {
                logger::warn("InteropCall: View ID [{}] not found.", viewId);
                return;
            }
        }

        Cef::CefRuntime::GetSingleton().InteropCallInView(viewId, functionName, argument);
    }

    // -------------------------------------------------------------------------
    // Dispatch helpers
    // -------------------------------------------------------------------------

    void DispatchListenerInvoke(uint64_t viewId, const std::string& name, std::string argument) {
        Core::SimpleJSCallback target;
        {
            std::lock_guard<std::mutex> lock(jsCallbacksMutex);
            auto it = jsCallbacks.find(std::make_pair(static_cast<Core::PrismaViewId>(viewId), name));
            if (it != jsCallbacks.end()) {
                target = it->second.callback;
            }
        }

        if (!target) {
            logger::warn("DispatchListenerInvoke: no callback registered for view [{}] / '{}'.", viewId, name);
            return;
        }
        try {
            target(argument);
        } catch (const std::exception& e) {
            logger::error("DispatchListenerInvoke: callback for view [{}] / '{}' threw: {}", viewId, name, e.what());
        } catch (...) {
            logger::error("DispatchListenerInvoke: callback for view [{}] / '{}' threw an unknown exception.",
                          viewId, name);
        }
    }

    void DispatchConsoleMessage(uint64_t viewId, const std::string& level, std::string text) {
        std::shared_ptr<PrismaView> viewData;
        {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it != views.end()) viewData = it->second;
        }
        if (!viewData || !viewData->consoleMessageCallback) {
            return;
        }

        const auto resolvedLevel = ParseConsoleLevel(level);
        try {
            viewData->consoleMessageCallback(static_cast<Core::PrismaViewId>(viewId), resolvedLevel, text);
        } catch (const std::exception& e) {
            logger::error("DispatchConsoleMessage: callback for view [{}] threw: {}", viewId, e.what());
        } catch (...) {
            logger::error("DispatchConsoleMessage: callback for view [{}] threw an unknown exception.", viewId);
        }
    }

    void DispatchDomReady(uint64_t viewId) {
        std::shared_ptr<PrismaView> viewData;
        {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it != views.end()) viewData = it->second;
        }
        if (!viewData) {
            logger::warn("DispatchDomReady: view [{}] not found.", viewId);
            return;
        }

        viewData->iframeReady.store(true, std::memory_order_release);
        logger::info("DispatchDomReady: view [{}] iframe DOM ready.", viewId);

        std::vector<std::string> listenerNames;
        {
            std::lock_guard<std::mutex> lock(jsCallbacksMutex);
            for (const auto& [key, data] : jsCallbacks) {
                if (key.first == viewId) {
                    listenerNames.push_back(data.name);
                }
            }
        }

        for (const auto& name : listenerNames) {
            Cef::CefRuntime::GetSingleton().RegisterListener(viewId, name, /*unused*/ nullptr);
        }


        if (viewData->domReadyCallback) {
            try {
                viewData->domReadyCallback(static_cast<Core::PrismaViewId>(viewId));
            } catch (const std::exception& e) {
                logger::error("DispatchDomReady: callback for view [{}] threw: {}", viewId, e.what());
            } catch (...) {
                logger::error("DispatchDomReady: callback for view [{}] threw an unknown exception.", viewId);
            }
        }
    }
}
