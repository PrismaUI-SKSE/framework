#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
    using SimpleJSCallback = std::function<void(const std::string&)>;
}

namespace PrismaUI::Communication {
    // Eval `script` in the iframe associated with `viewId`. `callback` (when provided)
    // is invoked exactly once with the coerced string result. On any failure path —
    // view unknown, iframe not yet attached, JS exception, view destroyed mid-flight —
    // the callback fires with an empty string.
    void Invoke(const Core::PrismaViewId& viewId, std::string script,
                std::function<void(std::string)> callback = nullptr);

    // Register a string-valued JS listener. The renderer installs window[name] so the
    // iframe can call it like a regular function; the call comes back into `callback`
    // with the argument coerced to string.
    void RegisterJSListener(const Core::PrismaViewId& viewId, const std::string& name,
                            Core::SimpleJSCallback callback);

    // Fire-and-forget call into the iframe's window[functionName](argument).
    void InteropCall(const Core::PrismaViewId& viewId, const std::string& functionName,
                     const std::string& argument);

    // -------------------------------------------------------------------------
    // Dispatch helpers called by CefRuntime when a renderer-process message
    // arrives. They look up Core::jsCallbacks / Core::views and invoke the
    // matching modder-provided callback. Public-facing entries already marshal
    // themselves onto the SKSE task interface, so it's safe to call these from
    // the CEF UI thread.
    // -------------------------------------------------------------------------
    void DispatchListenerInvoke(uint64_t viewId, const std::string& name, std::string argument);
    void DispatchConsoleMessage(uint64_t viewId, const std::string& level, std::string text);
    void DispatchDomReady(uint64_t viewId);
}
