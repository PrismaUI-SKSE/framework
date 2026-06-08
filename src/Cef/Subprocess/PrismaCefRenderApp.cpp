#include "Cef/Subprocess/PrismaCefRenderApp.h"

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Cef/Shared/ProcessMessageNames.h"
#include "Cef/Shared/ViewUtils.h"
#include "PrismaBootstrapScript.generated.h"
#include "include/base/cef_logging.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"
#include "include/cef_values.h"

namespace PrismaUI::Cef {
    namespace {
        // ----- helpers -----


        std::string V8ToString(CefRefPtr<CefV8Value> value) {
            if (!value) {
                return std::string();
            }
            if (value->IsUndefined() || value->IsNull()) {
                return std::string();
            }
            return value->GetStringValue().ToString();
        }

        const CefString& FireListenerName() {
            static const CefString value("fireListener");
            return value;
        }

        const CefString& FireDomReadyName() {
            static const CefString value("fireDomReady");
            return value;
        }

        const CefString& FireConsoleName() {
            static const CefString value("fireConsole");
            return value;
        }

        // Build a process message with the given (name, [string args]) shape.
        CefRefPtr<CefProcessMessage> MakeListMessage(Messages::RendererToBrowserMessage messageName,
                                                     std::initializer_list<std::string> args) {
            CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(Messages::ToCefString(messageName));
            CefRefPtr<CefListValue> list = msg->GetArgumentList();
            list->SetSize(args.size());
            size_t i = 0;
            for (const auto& a : args) {
                list->SetString(i++, a);
            }
            return msg;
        }

    }

    // ----- V8 handler that bridges __prismaNative.* calls back to the browser -----
    class PrismaNativeHandler : public CefV8Handler {
    public:
        explicit PrismaNativeHandler(std::string viewId) : viewId_(std::move(viewId)) {}

        bool Execute(const CefString& name, CefRefPtr<CefV8Value> /*object*/, const CefV8ValueList& arguments,
                     CefRefPtr<CefV8Value>& retval, CefString& /*exception*/) override {
            CefRefPtr<CefV8Context> ctx = CefV8Context::GetCurrentContext();
            CefRefPtr<CefFrame> frame = ctx ? ctx->GetFrame() : nullptr;
            if (!frame) {
                retval = CefV8Value::CreateUndefined();
                return true;
            }

            if (name == FireListenerName()) {
                if (arguments.size() < 1) {
                    retval = CefV8Value::CreateUndefined();
                    return true;
                }
                const std::string listenerName = V8ToString(arguments[0]);
                const std::string arg = arguments.size() >= 2 ? V8ToString(arguments[1]) : std::string();
                frame->SendProcessMessage(
                    PID_BROWSER,
                    MakeListMessage(Messages::RendererToBrowserMessage::ListenerInvoke, {viewId_, listenerName, arg}));
            } else if (name == FireDomReadyName()) {
                frame->SendProcessMessage(PID_BROWSER,
                                          MakeListMessage(Messages::RendererToBrowserMessage::DomReady, {viewId_}));
            } else if (name == FireConsoleName()) {
                if (arguments.size() < 2) {
                    retval = CefV8Value::CreateUndefined();
                    return true;
                }
                const std::string level = V8ToString(arguments[0]);
                const std::string text = V8ToString(arguments[1]);
                frame->SendProcessMessage(
                    PID_BROWSER,
                    MakeListMessage(Messages::RendererToBrowserMessage::ConsoleMessage, {viewId_, level, text}));
            } else {
                LOG(WARNING) << "PrismaCefRenderApp: unknown __prismaNative call '" << name.ToString() << "'";
            }

            retval = CefV8Value::CreateUndefined();
            return true;
        }

    private:
        const std::string viewId_;
        IMPLEMENT_REFCOUNTING(PrismaNativeHandler);
    };

    // ----- Impl -----
    struct PrismaCefRenderApp::Impl {
        struct FrameState {
            std::string viewId;
            CefRefPtr<CefV8Context> context;
            // Listener installs that arrived before OnContextCreated; replayed on next context.
            std::vector<std::string> pendingListeners;
        };

        std::mutex mutex;
        // Keyed by frame identifier (stable across the frame's lifetime).
        std::map<std::string, FrameState> frames;
    };

    PrismaCefRenderApp::PrismaCefRenderApp() : impl_(new Impl()) {}

    PrismaCefRenderApp::~PrismaCefRenderApp() { delete impl_; }

    // -------------------------------------------------------------------------
    // Install one window[name] = function(arg){__prismaNative.fireListener(...)}
    // trampoline inside an already-entered V8 context by calling the bootstrap
    // helper that the renderer installed once per context.
    // -------------------------------------------------------------------------
    static void InstallListenerTrampoline(CefRefPtr<CefV8Context> context, const std::string& name) {
        if (!context || !context->IsValid()) {
            return;
        }
        CefRefPtr<CefV8Value> global = context->GetGlobal();
        if (!global) {
            return;
        }
        CefRefPtr<CefV8Value> installer = global->GetValue("__prismaInstallListener");
        if (!installer || !installer->IsFunction()) {
            LOG(ERROR) << "PrismaCefRenderApp: bootstrap installer missing while binding '" << name << "'";
            return;
        }
        CefV8ValueList args;
        args.push_back(CefV8Value::CreateString(name));
        installer->ExecuteFunction(nullptr, args);
        if (installer->HasException()) {
            CefRefPtr<CefV8Exception> ex = installer->GetException();
            LOG(ERROR) << "PrismaCefRenderApp: install listener '" << name
                       << "' threw: " << (ex ? ex->GetMessage().ToString() : std::string{});
        }
    }

    void PrismaCefRenderApp::OnContextCreated(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                              CefRefPtr<CefV8Context> context) {
        if (!frame || !context) {
            return;
        }

        const std::string frameName = frame->GetName().ToString();
        std::uint64_t parsedViewId = 0;
        if (!ViewUtils::TryParseViewIdFromFrameName(frameName, parsedViewId)) {
            // Not one of our iframes — leave the context untouched. The shell
            // frame and other helpers do not need the bridge.
            return;
        }

        const std::string viewId = frameName;
        const std::string frameIdentifier = frame->GetIdentifier().ToString();

        std::vector<std::string> pending;
        {
            std::lock_guard lock(impl_->mutex);
            auto& state = impl_->frames[frameIdentifier];
            state.viewId = viewId;
            state.context = context;
            pending.swap(state.pendingListeners);
        }

        if (!context->Enter()) {
            LOG(ERROR) << "PrismaCefRenderApp: failed to enter V8 context for view " << viewId;
            return;
        }

        // Install the __prismaNative global with three native functions.
        CefRefPtr<PrismaNativeHandler> handler = new PrismaNativeHandler(viewId);
        CefRefPtr<CefV8Value> native = CefV8Value::CreateObject(nullptr, nullptr);
        native->SetValue("fireListener", CefV8Value::CreateFunction("fireListener", handler),
                         V8_PROPERTY_ATTRIBUTE_READONLY);
        native->SetValue("fireDomReady", CefV8Value::CreateFunction("fireDomReady", handler),
                         V8_PROPERTY_ATTRIBUTE_READONLY);
        native->SetValue("fireConsole", CefV8Value::CreateFunction("fireConsole", handler),
                         V8_PROPERTY_ATTRIBUTE_READONLY);
        native->SetValue("imeFocusListenerName", CefV8Value::CreateString(Messages::kImeFocusListener),
                         V8_PROPERTY_ATTRIBUTE_READONLY);

        CefRefPtr<CefV8Value> global = context->GetGlobal();
        global->SetValue(
            "__prismaNative", native,
            static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTENUM |
                                                       V8_PROPERTY_ATTRIBUTE_DONTDELETE));

        // Run the bootstrap script: console wrappers + DOM-ready dispatch.
        CefRefPtr<CefV8Value> retval;
        CefRefPtr<CefV8Exception> exception;
        if (!context->Eval(CefString(kBootstrapScript), CefString("prismaui://bootstrap"), 0, retval, exception)) {
            if (exception) {
                LOG(ERROR) << "PrismaCefRenderApp: bootstrap failed for view " << viewId << ": "
                           << exception->GetMessage().ToString();
            }
        }

        // Replay any queued listener installs.
        for (const auto& name : pending) {
            InstallListenerTrampoline(context, name);
        }

        context->Exit();

        LOG(INFO) << "PrismaCefRenderApp: bridge installed for view " << viewId << " (frame " << frameIdentifier << ")";
    }

    void PrismaCefRenderApp::OnContextReleased(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                               CefRefPtr<CefV8Context> /*context*/) {
        if (!frame) {
            return;
        }
        const std::string frameIdentifier = frame->GetIdentifier().ToString();
        std::lock_guard lock(impl_->mutex);
        impl_->frames.erase(frameIdentifier);
    }

    bool PrismaCefRenderApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                                      CefProcessId source_process,
                                                      CefRefPtr<CefProcessMessage> message) {
        if (source_process != PID_BROWSER || !frame || !message) {
            return false;
        }

        const auto name = Messages::ClassifyBrowserToRendererMessage(message->GetName());
        if (name == Messages::BrowserToRendererMessage::Unknown) {
            return false;
        }

        CefRefPtr<CefListValue> args = message->GetArgumentList();
        if (!args) {
            return false;
        }

        const std::string frameIdentifier = frame->GetIdentifier().ToString();

        if (name == Messages::BrowserToRendererMessage::InstallListener) {
            if (args->GetSize() < 2) {
                LOG(ERROR) << "PrismaCefRenderApp: " << Messages::ToStringView(name) << " missing args";
                return true;
            }
            const std::string listenerName = args->GetString(1).ToString();

            CefRefPtr<CefV8Context> context;
            {
                std::lock_guard lock(impl_->mutex);
                auto it = impl_->frames.find(frameIdentifier);
                if (it != impl_->frames.end() && it->second.context) {
                    context = it->second.context;
                } else {
                    // Queue for next OnContextCreated.
                    impl_->frames[frameIdentifier].pendingListeners.push_back(listenerName);
                    LOG(INFO) << "PrismaCefRenderApp: queued listener '" << listenerName << "' until V8 context ready";
                    return true;
                }
            }

            if (context->Enter()) {
                InstallListenerTrampoline(context, listenerName);
                context->Exit();
                LOG(INFO) << "PrismaCefRenderApp: installed listener '" << listenerName << "'";
            }
            return true;
        }

        if (name == Messages::BrowserToRendererMessage::RemoveListener) {
            if (args->GetSize() < 2) {
                LOG(ERROR) << "PrismaCefRenderApp: " << Messages::ToStringView(name) << " missing args";
                return true;
            }
            const std::string listenerName = args->GetString(1).ToString();
            CefRefPtr<CefV8Context> context;
            {
                std::lock_guard lock(impl_->mutex);
                auto it = impl_->frames.find(frameIdentifier);
                if (it != impl_->frames.end() && it->second.context) {
                    context = it->second.context;
                }
            }
            if (context && context->Enter()) {
                CefRefPtr<CefV8Value> global = context->GetGlobal();
                if (global) {
                    global->DeleteValue(listenerName);
                }
                context->Exit();
                LOG(INFO) << "PrismaCefRenderApp: removed listener '" << listenerName << "'";
            }
            return true;
        }

        if (name == Messages::BrowserToRendererMessage::InvokeRequest) {
            if (args->GetSize() < 2) {
                LOG(ERROR) << "PrismaCefRenderApp: " << Messages::ToStringView(name) << " missing args";
                return true;
            }
            const std::string requestId = args->GetString(0).ToString();
            const std::string script = args->GetString(1).ToString();

            std::string okStr = "0";
            std::string resultStr;

            CefRefPtr<CefV8Context> context;
            {
                std::lock_guard lock(impl_->mutex);
                auto it = impl_->frames.find(frameIdentifier);
                if (it != impl_->frames.end() && it->second.context) {
                    context = it->second.context;
                }
            }
            if (context && context->Enter()) {
                CefRefPtr<CefV8Value> retval;
                CefRefPtr<CefV8Exception> exception;
                const bool ok = context->Eval(CefString(script), CefString("prismaui://invoke"), 0, retval, exception);
                if (ok && retval) {
                    if (retval->IsUndefined() || retval->IsNull()) {
                        resultStr.clear();
                    } else if (retval->IsString()) {
                        resultStr = retval->GetStringValue().ToString();
                    } else {
                        // Coerce non-string results via JSON.stringify when possible. For
                        // values JSON cannot represent (functions, cycles, undefined inside
                        // arrays) we leave the result empty — matches the legacy Ultralight
                        // path which delivered an empty string on coercion failure.
                        CefRefPtr<CefV8Value> json = context->GetGlobal()->GetValue("JSON");
                        CefRefPtr<CefV8Value> jsonStringify = json ? json->GetValue("stringify") : nullptr;
                        if (jsonStringify && jsonStringify->IsFunction()) {
                            CefV8ValueList stringifyArgs;
                            stringifyArgs.push_back(retval);
                            CefRefPtr<CefV8Value> stringified = jsonStringify->ExecuteFunction(nullptr, stringifyArgs);
                            if (stringified && stringified->IsString()) {
                                resultStr = stringified->GetStringValue().ToString();
                            }
                        }
                    }
                    okStr = "1";
                } else if (exception) {
                    LOG(WARNING) << "PrismaCefRenderApp: Invoke exception: " << exception->GetMessage().ToString();
                }
                context->Exit();
            } else {
                LOG(WARNING) << "PrismaCefRenderApp: Invoke arrived without an entered V8 context";
            }

            frame->SendProcessMessage(PID_BROWSER, MakeListMessage(Messages::RendererToBrowserMessage::InvokeResult,
                                                                   {requestId, okStr, resultStr}));
            return true;
        }

        if (name == Messages::BrowserToRendererMessage::InteropCall) {
            if (args->GetSize() < 2) {
                LOG(ERROR) << "PrismaCefRenderApp: " << Messages::ToStringView(name) << " missing args";
                return true;
            }
            const std::string functionName = args->GetString(0).ToString();
            const std::string argument = args->GetString(1).ToString();

            CefRefPtr<CefV8Context> context;
            {
                std::lock_guard lock(impl_->mutex);
                auto it = impl_->frames.find(frameIdentifier);
                if (it != impl_->frames.end() && it->second.context) {
                    context = it->second.context;
                }
            }
            if (context && context->Enter()) {
                CefRefPtr<CefV8Value> global = context->GetGlobal();
                CefRefPtr<CefV8Value> fn = global ? global->GetValue(functionName) : nullptr;
                if (fn && fn->IsFunction()) {
                    CefV8ValueList callArgs;
                    callArgs.push_back(CefV8Value::CreateString(argument));
                    fn->ExecuteFunction(nullptr, callArgs);
                    if (fn->HasException()) {
                        CefRefPtr<CefV8Exception> ex = fn->GetException();
                        LOG(WARNING) << "PrismaCefRenderApp: InteropCall '" << functionName
                                     << "' threw: " << (ex ? ex->GetMessage().ToString() : std::string{});
                    }
                } else {
                    LOG(WARNING) << "PrismaCefRenderApp: InteropCall target '" << functionName << "' is not a function";
                }
                context->Exit();
            }
            return true;
        }

        return false;
    }
}
