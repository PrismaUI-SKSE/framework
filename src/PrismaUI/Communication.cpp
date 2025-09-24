#include "Communication.h"
#include "Core.h"
#include "ViewManager.h"

namespace PrismaUI::Communication {
	using namespace Core;
	using namespace ViewManager;

	void Invoke(const Core::PrismaViewId& viewId, const String& script, std::function<void(std::string)> callback) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			logger::warn("Invoke: View ID [{}] not found.", viewId);
			if (callback) {
				callback("");
			}
			return;
		}

		ultralightThread.submit([view_ptr = viewData->ultralightView, script_copy = script, callback]() {
			String result = "";
			if (view_ptr) {
				try {
					result = view_ptr->EvaluateScript(script_copy);
				}
				catch (const std::exception& e) {
					logger::error("Exception during EvaluateScript: {}", e.what());
				}
				catch (...) {
					logger::error("Unknown exception during EvaluateScript");
				}
			}

			if (callback) {
				callback(result.utf8().data());
			}
			});
	}

	void RegisterJSListener(const Core::PrismaViewId& viewId, const std::string& name, Core::SimpleJSCallback callback) {
		if (!ViewManager::IsValid(viewId)) {
			logger::error("RegisterJSListener: View ID [{}] not found.", viewId);
			return;
		}

		{
			std::lock_guard<std::mutex> lock(jsCallbacksMutex);
			JSCallbackData data;
			data.viewId = viewId;
			data.name = name;
			data.callback = callback;
			jsCallbacks[std::make_pair(viewId, name)] = std::move(data);
			logger::debug("RegisterJSListener: Registered callback '{}' for view [{}]", name, viewId);
		}

		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (viewData && viewData->ultralightView && viewData->isLoadingFinished) {
			ultralightThread.submit([viewId, name]() {
				BindJSCallbacks(viewId);
				});
		}
	}

	void BindJSCallbacks(const Core::PrismaViewId& viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData || !viewData->ultralightView || !viewData->isLoadingFinished) {
			logger::warn("BindJSCallbacks: View [{}] not ready or not loaded.", viewId);
			return;
		}

		std::vector<JSCallbackData> viewCallbacks;
		{
			std::lock_guard<std::mutex> lock(jsCallbacksMutex);
			for (const auto& pair : jsCallbacks) {
				if (pair.first.first == viewId) {
					viewCallbacks.push_back(pair.second);
				}
			}
		}

		if (viewCallbacks.empty()) {
			return;
		}

		auto scoped_context = viewData->ultralightView->LockJSContext();
		JSContextRef ctx = (*scoped_context);
		JSObjectRef globalObj = JSContextGetGlobalObject(ctx);

		for (const auto& callbackData : viewCallbacks) {
			logger::debug("BindJSCallbacks: Binding callback '{}' for view [{}]", callbackData.name, callbackData.viewId);

			JSObjectRef dataObj = JSObjectMake(ctx, nullptr, nullptr);

			JSStringRef viewIdKey = JSStringCreateWithUTF8CString("viewId");
			JSStringRef nameKey = JSStringCreateWithUTF8CString("name");

			JSStringRef viewIdValue = JSStringCreateWithUTF8CString(std::to_string(callbackData.viewId).c_str());
			JSStringRef nameValue = JSStringCreateWithUTF8CString(callbackData.name.c_str());

			JSObjectSetProperty(ctx, dataObj, viewIdKey, JSValueMakeString(ctx, viewIdValue),
				kJSPropertyAttributeReadOnly, nullptr);
			JSObjectSetProperty(ctx, dataObj, nameKey, JSValueMakeString(ctx, nameValue),
				kJSPropertyAttributeReadOnly, nullptr);

			JSStringRelease(viewIdKey);
			JSStringRelease(nameKey);
			JSStringRelease(viewIdValue);
			JSStringRelease(nameValue);

			JSStringRef funcJS = JSStringCreateWithUTF8CString(callbackData.name.c_str());

			JSObjectRef funcObj = JSObjectMakeFunctionWithCallback(ctx, funcJS, InvokeCppCallback);

			JSStringRef dataKey = JSStringCreateWithUTF8CString("data");
			JSObjectSetProperty(ctx, funcObj, dataKey, dataObj, kJSPropertyAttributeReadOnly, nullptr);
			JSStringRelease(dataKey);

			JSObjectSetProperty(ctx, globalObj, funcJS, funcObj, kJSPropertyAttributeNone, nullptr);

			JSStringRelease(funcJS);

			logger::debug("BindJSCallbacks: Successfully bound callback '{}' for view [{}]",
				callbackData.name, callbackData.viewId);
		}
	}


	void InteropCall(const Core::PrismaViewId& viewId, const std::string& functionName, const std::string& argument) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			logger::warn("InteropCall: View ID [{}] not found.", viewId);
			return;
		}

		if (!viewData->ultralightView) {
			logger::warn("InteropCall: View ID [{}] has no Ultralight view object.", viewId);
			return;
		}

		ultralightThread.submit([view_ptr = viewData->ultralightView, funcName = functionName, arg = argument, viewId]() {
			if (!view_ptr) {
				return;
			}

			auto scoped_context = view_ptr->LockJSContext();
			JSContextRef ctx = (*scoped_context);
			JSValueRef exception = nullptr;

			JSObjectRef globalObj = JSContextGetGlobalObject(ctx);

			JSRetainPtr<JSStringRef> funcNameStr = adopt(JSStringCreateWithUTF8CString(funcName.c_str()));
			JSValueRef funcValue = JSObjectGetProperty(ctx, globalObj, funcNameStr.get(), &exception);

			if (exception) {
				JSStringRef exceptionStr = JSValueToStringCopy(ctx, exception, nullptr);
				size_t bufferSize = JSStringGetMaximumUTF8CStringSize(exceptionStr);
				std::vector<char> buffer(bufferSize);
				JSStringGetUTF8CString(exceptionStr, buffer.data(), bufferSize);
				logger::error("InteropCall [{}]: Exception getting function '{}': {}", viewId, funcName, buffer.data());
				JSStringRelease(exceptionStr);
				return;
			}

			if (JSValueIsObject(ctx, funcValue)) {
				JSObjectRef funcObj = JSValueToObject(ctx, funcValue, nullptr);

				if (funcObj && JSObjectIsFunction(ctx, funcObj)) {
					JSRetainPtr<JSStringRef> argStr = adopt(JSStringCreateWithUTF8CString(arg.c_str()));
					const JSValueRef args[] = { JSValueMakeString(ctx, argStr.get()) };

					JSObjectCallAsFunction(ctx, funcObj, globalObj, 1, args, &exception);

					if (exception) {
						JSStringRef exceptionStr = JSValueToStringCopy(ctx, exception, nullptr);
						size_t bufferSize = JSStringGetMaximumUTF8CStringSize(exceptionStr);
						std::vector<char> buffer(bufferSize);
						JSStringGetUTF8CString(exceptionStr, buffer.data(), bufferSize);
						logger::error("InteropCall [{}]: Exception calling function '{}': {}", viewId, funcName, buffer.data());
						JSStringRelease(exceptionStr);
					}
				}
				else {
					logger::warn("InteropCall [{}]: Global property '{}' is not a function.", viewId, funcName);
				}
			}
			else {
				logger::warn("InteropCall [{}]: Global property '{}' not found or not an object.", viewId, funcName);
			}
			});
	}

	JSValueRef InvokeCppCallback(JSContextRef ctx, JSObjectRef function,
		JSObjectRef thisObject, size_t argumentCount,
		const JSValueRef arguments[], JSValueRef* exception) {

		logger::debug("InvokeCppCallback: Called from JavaScript");

		JSValueRef dataValue = JSObjectGetProperty(ctx, function,
			JSStringCreateWithUTF8CString("data"), exception);

		if (!dataValue || JSValueIsNull(ctx, dataValue) || JSValueIsUndefined(ctx, dataValue)) {
			logger::error("InvokeCppCallback: No data attached to the function");
			return JSValueMakeUndefined(ctx);
		}

		JSStringRef viewIdKey = JSStringCreateWithUTF8CString("viewId");
		JSStringRef nameKey = JSStringCreateWithUTF8CString("name");

		JSObjectRef dataObj = JSValueToObject(ctx, dataValue, exception);
		if (!dataObj) {
			logger::error("InvokeCppCallback: Failed to convert data to object");
			JSStringRelease(viewIdKey);
			JSStringRelease(nameKey);
			return JSValueMakeUndefined(ctx);
		}

		JSValueRef viewIdValue = JSObjectGetProperty(ctx, dataObj, viewIdKey, exception);
		JSValueRef nameValue = JSObjectGetProperty(ctx, dataObj, nameKey, exception);

		JSStringRelease(viewIdKey);
		JSStringRelease(nameKey);

		if (!viewIdValue || !nameValue) {
			logger::error("InvokeCppCallback: Failed to get viewId or name from data object");
			return JSValueMakeUndefined(ctx);
		}

		JSStringRef viewIdStr = JSValueToStringCopy(ctx, viewIdValue, exception);
		JSStringRef nameStr = JSValueToStringCopy(ctx, nameValue, exception);

		if (!viewIdStr || !nameStr) {
			logger::error("InvokeCppCallback: Failed to convert viewId or name to string");
			if (viewIdStr) JSStringRelease(viewIdStr);
			if (nameStr) JSStringRelease(nameStr);
			return JSValueMakeUndefined(ctx);
		}

		size_t viewIdLen = JSStringGetMaximumUTF8CStringSize(viewIdStr);
		size_t nameLen = JSStringGetMaximumUTF8CStringSize(nameStr);

		std::vector<char> viewIdBuffer(viewIdLen);
		std::vector<char> nameBuffer(nameLen);

		JSStringGetUTF8CString(viewIdStr, viewIdBuffer.data(), viewIdLen);
		JSStringGetUTF8CString(nameStr, nameBuffer.data(), nameLen);

		JSStringRelease(viewIdStr);
		JSStringRelease(nameStr);

		Core::PrismaViewId viewId = std::stoull(std::string(viewIdBuffer.data()));
		std::string name(nameBuffer.data());

		logger::debug("InvokeCppCallback: Looking for callback viewId={}, name={}", viewId, name);

		std::string paramStr;
		if (argumentCount > 0) {
			JSStringRef jsStrParam = JSValueToStringCopy(ctx, arguments[0], exception);
			if (jsStrParam) {
				size_t paramBufferSize = JSStringGetMaximumUTF8CStringSize(jsStrParam);
				std::vector<char> paramBuffer(paramBufferSize);
				JSStringGetUTF8CString(jsStrParam, paramBuffer.data(), paramBufferSize);
				JSStringRelease(jsStrParam);
				paramStr = paramBuffer.data();
			}
		}

		Core::SimpleJSCallback targetCallback = nullptr;
		{
			std::lock_guard<std::mutex> lock(PrismaUI::Core::jsCallbacksMutex);
			auto it = PrismaUI::Core::jsCallbacks.find(std::make_pair(viewId, name));
			if (it != PrismaUI::Core::jsCallbacks.end()) {
				targetCallback = it->second.callback;
			}
		}

		if (targetCallback) {
			logger::debug("InvokeCppCallback: Found callback. Invoking with data: '{}'", paramStr);
			try {
				targetCallback(paramStr);
				logger::debug("InvokeCppCallback: Callback invoked successfully");
			}
			catch (const std::exception& e) {
				logger::error("InvokeCppCallback: Exception in callback: {}", e.what());
				if (exception) {
					JSStringRef errStr = JSStringCreateWithUTF8CString(e.what());
					*exception = JSValueMakeString(ctx, errStr);
					JSStringRelease(errStr);
				}
			}
			catch (...) {
				logger::error("InvokeCppCallback: Unknown exception in callback");
				if (exception) {
					JSStringRef errStr = JSStringCreateWithUTF8CString("Unknown error in C++ callback");
					*exception = JSValueMakeString(ctx, errStr);
					JSStringRelease(errStr);
				}
			}
		}
		else {
			logger::error("InvokeCppCallback: Callback not found for viewId={}, name={}", viewId, name);
			if (exception) {
				std::string errMsg = "C++ callback not found: " + name + " for view " + std::to_string(viewId);
				JSStringRef errorStr = JSStringCreateWithUTF8CString(errMsg.c_str());
				*exception = JSValueMakeString(ctx, errorStr);
				JSStringRelease(errorStr);
			}
		}

		return JSValueMakeUndefined(ctx);
	}
}
