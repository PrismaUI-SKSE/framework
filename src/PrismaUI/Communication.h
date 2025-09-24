#pragma once

#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#include <Ultralight/StringSTL.h>
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>

namespace PrismaUI::Core {
	typedef uint64_t PrismaViewId;
	using SimpleJSCallback = std::function<void(std::string)>;
}

namespace PrismaUI::Communication {
	using namespace ultralight;

	void Invoke(const Core::PrismaViewId& viewId, const String& script, std::function<void(std::string)> callback = nullptr);
	void BindJSCallbacks(const Core::PrismaViewId& viewId);
	JSValueRef InvokeCppCallback(JSContextRef ctx, JSObjectRef function,
		JSObjectRef thisObject, size_t argumentCount,
		const JSValueRef arguments[], JSValueRef* exception);
	void RegisterJSListener(const Core::PrismaViewId& viewId, const std::string& name, Core::SimpleJSCallback callback);
	void InteropCall(const Core::PrismaViewId& viewId, const std::string& functionName, const std::string& argument);
}
