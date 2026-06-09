#include "PrismaCefRenderApp.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "Cef/Shared/BrowserToRendererMessages.h"
#include "Cef/Shared/CefUtils.h"
#include "Cef/Shared/Messaging.h"
#include "Cef/Shared/ProcessMessageNames.h"
#include "Cef/Shared/RendererToBrowserMessages.h"
#include "Cef/Shared/ViewUtils.h"
#include "PrismaBootstrapScript.generated.h"
#include "Utils/VariantUtils.h"
#include "include/base/cef_logging.h"
#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"
#include "include/cef_values.h"

namespace PrismaUI::Cef {
    namespace {
        // ----- helpers -----

        CefString V8ToCefString(CefRefPtr<CefV8Value> value) {
            if (!value) {
                return CefString();
            }
            if (value->IsUndefined() || value->IsNull()) {
                return CefString();
            }
            return value->GetStringValue();
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
    }

    // ----- V8 handler that bridges __prismaNative.* calls back to the browser -----
    class PrismaNativeHandler : public CefV8Handler {
    public:
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
                const CefString listenerName = V8ToCefString(arguments[0]);
                const CefString arg = arguments.size() >= 2 ? V8ToCefString(arguments[1]) : CefString();
                Messaging::SendProcessMessageToFrame(frame, PID_BROWSER,
                                                     RTBMessages::ListenerInvokeMessage{listenerName, arg});
            } else if (name == FireDomReadyName()) {
                Messaging::SendProcessMessageToFrame(frame, PID_BROWSER, RTBMessages::DomReadyMessage{});
            } else if (name == FireConsoleName()) {
                if (arguments.size() < 2) {
                    retval = CefV8Value::CreateUndefined();
                    return true;
                }
                const CefString level = V8ToCefString(arguments[0]);
                const CefString text = V8ToCefString(arguments[1]);
                Messaging::SendProcessMessageToFrame(frame, PID_BROWSER, RTBMessages::ConsoleMessage{level, text});
            } else {
                LOG(WARNING) << "PrismaCefRenderApp: unknown __prismaNative call '" << name.ToString() << "'";
            }

            retval = CefV8Value::CreateUndefined();
            return true;
        }

        IMPLEMENT_REFCOUNTING(PrismaNativeHandler);
    };

    // ----- Impl -----
    struct PrismaCefRenderApp::Impl {
        struct FrameState {
            CefString viewId;
            CefRefPtr<CefV8Context> context;
            // Listener installs that arrived before OnContextCreated; replayed on next context.
            std::vector<CefString> pendingListeners;
        };

        // Keyed by frame identifier (stable across the frame's lifetime).
        std::map<CefString, FrameState> frames;
    };

    PrismaCefRenderApp::PrismaCefRenderApp() : impl_(new Impl()) {}

    PrismaCefRenderApp::~PrismaCefRenderApp() { delete impl_; }

    // -------------------------------------------------------------------------
    // Install one window[name] = function(arg){__prismaNative.fireListener(...)}
    // trampoline inside an already-entered V8 context by calling the bootstrap
    // helper that the renderer installed once per context.
    // -------------------------------------------------------------------------
    static void InstallListenerTrampoline(const CefRefPtr<CefV8Context>& context, const CefString& name) {
        if (!context || !context->IsValid()) {
            return;
        }

        CefRefPtr<CefV8Value> global = context->GetGlobal();
        if (!global) {
            return;
        }

        CefRefPtr<CefV8Value> installer = global->GetValue("__prismaInstallListener");
        CefV8ValueList args;
        args.push_back(CefV8Value::CreateString(name));
        CefUtils::ExecuteFunction(installer, args);
    }

    void PrismaCefRenderApp::OnContextCreated(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                              CefRefPtr<CefV8Context> context) {
        if (!frame || !context) {
            return;
        }

        const CefString frameName = frame->GetName();

        if (frameName.empty()) {
            return;
        }

        std::uint64_t viewId = 0;
        if (!ViewUtils::TryParseViewIdFromFrameName(frameName.ToString(), viewId)) {
            return;
        }

        std::vector<CefString> pending;
        {
            auto& state = impl_->frames[frameName];
            state.viewId = frameName;
            state.context = context;
            pending.swap(state.pendingListeners);
        }

        CefUtils::EnterContext(context, [&frameName, &pending](const CefRefPtr<CefV8Context>& context) {
            // Install the __prismaNative global with three native functions.
            CefRefPtr handler = new PrismaNativeHandler();
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
            global->SetValue("__prismaNative", native,
                             static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY |
                                                                        V8_PROPERTY_ATTRIBUTE_DONTENUM |
                                                                        V8_PROPERTY_ATTRIBUTE_DONTDELETE));

            // Run the bootstrap script: console wrappers + DOM-ready dispatch.
            CefRefPtr<CefV8Value> retval;
            CefRefPtr<CefV8Exception> exception;
            if (!context->Eval(CefString(kBootstrapScript), CefString("prismaui://bootstrap"), 0, retval, exception)) {
                if (exception) {
                    LOG(ERROR) << "PrismaCefRenderApp: bootstrap failed for view " << frameName << ": "
                               << exception->GetMessage().ToString();
                }
            }

            // Replay any queued listener installs.
            for (const auto& name : pending) {
                InstallListenerTrampoline(context, name);
            }

            LOG(INFO) << "PrismaCefRenderApp: bridge installed for view " << frameName;
        });
    }

    void PrismaCefRenderApp::OnContextReleased(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                               CefRefPtr<CefV8Context> /*context*/) {
        if (!frame) {
            return;
        }
        const CefString frameName = frame->GetName();
        impl_->frames.erase(frameName);
    }

    bool PrismaCefRenderApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                                      CefProcessId source_process,
                                                      CefRefPtr<CefProcessMessage> message) {
        if (source_process != PID_BROWSER || !frame || !message) {
            return false;
        }

        const CefString frameName = frame->GetName();
        auto deserializeResult =
            Messaging::DeserializeProcessMessageVariant<BTRMessages::BrowserToRendererMessage>(message);

        return Match(
            deserializeResult,
            [&](const BTRMessages::BrowserToRendererMessage& parsedMessage) {
                auto frameIt = impl_->frames.find(frameName);
                auto invalidFrameIt = impl_->frames.end();

                if (auto installListenerMessage = std::get_if<BTRMessages::InstallListenerMessage>(&parsedMessage)) {
                    if (frameIt == invalidFrameIt) {
                        // Queue for next OnContextCreated.
                        impl_->frames[frameName].pendingListeners.emplace_back(installListenerMessage->ListenerName);
                        LOG(INFO) << "PrismaCefRenderApp: queued listener '" << installListenerMessage->ListenerName
                                  << "' until V8 context ready";
                    } else {
                        CefRefPtr<CefV8Context> context = frameIt->second.context;
                        CefUtils::EnterContext(
                            context, [installListenerMessage](const CefRefPtr<CefV8Context>& context) {
                                InstallListenerTrampoline(context, installListenerMessage->ListenerName);
                                LOG(INFO) << "PrismaCefRenderApp: installed listener '"
                                          << installListenerMessage->ListenerName << "'";
                            });
                    }

                    return true;
                }

                if (frameIt == invalidFrameIt) {
                    LOG(ERROR) << "PrismaCefRenderApp: Frame " << frameName << " not found in OnProcessMessageReceived";
                    return false;
                }

                CefRefPtr<CefV8Context> context = frameIt->second.context;
                CefUtils::EnterContext(context, [&parsedMessage, &frame](const CefRefPtr<CefV8Context>& context) {
                    Match(
                        parsedMessage, [](const BTRMessages::InstallListenerMessage&) {},
                        [&context](const BTRMessages::RemoveListenerMessage& m) {
                            if (CefRefPtr<CefV8Value> global = context->GetGlobal()) {
                                global->DeleteValue(m.ListenerName);
                            }

                            LOG(INFO) << "PrismaCefRenderApp: removed listener '" << m.ListenerName << "'";
                        },
                        [&context, &frame](const BTRMessages::InvokeRequestMessage& m) {
                            CefRefPtr<CefV8Value> retval;
                            CefRefPtr<CefV8Exception> exception;
                            bool success = false;
                            CefString resultStr;
                            const bool ok =
                                context->Eval(m.Script, CefString("prismaui://invoke"), 0, retval, exception);

                            if (ok && retval) {
                                if (retval->IsUndefined() || retval->IsNull()) {
                                    resultStr.clear();
                                } else if (retval->IsString()) {
                                    resultStr = retval->GetStringValue();
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
                                        CefRefPtr<CefV8Value> stringified =
                                            jsonStringify->ExecuteFunction(nullptr, stringifyArgs);
                                        if (stringified && stringified->IsString()) {
                                            resultStr = stringified->GetStringValue();
                                        }
                                    }
                                }
                                success = true;
                            } else if (exception) {
                                LOG(WARNING) << "PrismaCefRenderApp: Invoke exception: " << exception->GetMessage();
                            }

                            Messaging::SendProcessMessageToFrame(frame, PID_BROWSER,
                                                                 RTBMessages::InvokeResultMessage{
                                                                     .Result = std::move(resultStr),
                                                                     .RequestId = m.RequestId,
                                                                     .Success = success,
                                                                 });
                        },
                        [&context](const BTRMessages::InteropCallMessage& m) {
                            CefRefPtr<CefV8Value> global = context->GetGlobal();
                            CefRefPtr<CefV8Value> fn = global ? global->GetValue(m.FunctionName) : nullptr;
                            CefV8ValueList callArgs;
                            callArgs.push_back(CefV8Value::CreateString(m.Argument));
                            CefUtils::ExecuteFunction(fn, callArgs);
                        });
                });

                return true;
            },
            [&](const Messaging::DeserializeMessageError&) {
                LOG(WARNING) << "PrismaCefRenderApp failed to deserialize browser message, frame: " << frameName
                             << ", message: " << message->GetName();
                return false;
            });
    }
}
