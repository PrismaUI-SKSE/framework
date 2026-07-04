#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/StringSTL.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
    using SimpleJSCallback = std::function<void(std::string)>;
}

namespace PrismaUI::Communication {
    void Invoke(const Core::PrismaViewId& viewId, const ultralight::String& script,
                std::function<void(std::string)> callback = nullptr);
    // Call only from Ultralight threads. Otherwise, use Invoke.
    // This is a synchronous variant of Invoke.
    void InvokeFromUltralightThread(const Core::PrismaViewId& viewId, const ultralight::String& script);
    void BindJSCallbacks(const Core::PrismaViewId& viewId);
    JSValueRef InvokeCppCallback(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject, size_t argumentCount,
                                 const JSValueRef arguments[], JSValueRef* exception);
    void RegisterJSListener(const Core::PrismaViewId& viewId, const std::string& name, Core::SimpleJSCallback callback);
    void InteropCall(const Core::PrismaViewId& viewId, const std::string& functionName, const std::string& argument);
    JSValueRef JSCallbackDispatcher(JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
                                    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception);
}
