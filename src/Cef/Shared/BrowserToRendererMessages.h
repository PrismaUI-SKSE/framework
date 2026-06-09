#pragma once
#include <optional>
#include <variant>

#include "CefUtils.h"
#include "cef_process_message.h"
#include "cef_values.h"
#include "include/internal/cef_string.h"

// Stable IPC message names sent from the browser process (PrismaUI.dll)
// to the CEF renderer subprocess (PrismaUICefSubprocess.exe). These strings
// MUST NOT change without updating both sides.

namespace PrismaUI::Cef::Messages {
    inline const CefString& InstallListenerName() {
        static const CefString value("prisma.installListener");
        return value;
    }

    inline const CefString& RemoveListenerName() {
        static const CefString value("prisma.removeListener");
        return value;
    }

    inline const CefString& InvokeRequestName() {
        static const CefString value("prisma.invokeRequest");
        return value;
    }

    inline const CefString& InteropCallName() {
        static const CefString value("prisma.interopCall");
        return value;
    }

    inline CefRefPtr<CefProcessMessage> CreateInstallListenerMessage(const CefString& listenerName) {
        return CefUtils::MakeListMessage(InstallListenerName(), listenerName);
    }

    inline CefRefPtr<CefProcessMessage> CreateRemoveListenerMessage(const CefString& listenerName) {
        return CefUtils::MakeListMessage(RemoveListenerName(), listenerName);
    }

    inline CefRefPtr<CefProcessMessage> CreateInvokeRequestMessage(std::uint64_t requestId, const CefString& script) {
        return CefUtils::MakeListMessage(InvokeRequestName(), requestId, script);
    }

    inline CefRefPtr<CefProcessMessage> CreateInteropCallMessage(const CefString& functionName,
                                                                 const CefString& argument) {
        return CefUtils::MakeListMessage(InteropCallName(), functionName, argument);
    }

    struct InstallListenerMessage {
        CefString ListenerName;
    };

    struct RemoveListenerMessage {
        CefString ListenerName;
    };

    struct InvokeRequestMessage {
        std::uint64_t RequestId;
        CefString Script;
    };

    struct InteropCallMessage {
        CefString FunctionName;
        CefString Argument;
    };

    using BrowserToRendererMessage =
        std::variant<InstallListenerMessage, RemoveListenerMessage, InvokeRequestMessage, InteropCallMessage>;

    inline std::optional<BrowserToRendererMessage> ParseBrowserToRendererMessage(
        const CefRefPtr<CefProcessMessage>& message) {
        const CefString name = message->GetName();
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        if (!args) {
            LOG(ERROR) << "ParseBrowserToRendererMessage: " << name << " args pointer is null";
            return std::nullopt;
        }

        auto validateArgs = [](const CefRefPtr<CefListValue>& args, const CefString& name, int requiredCount) {
            if (args->GetSize() < requiredCount) {
                LOG(ERROR) << "ParseBrowserToRendererMessage: " << name << " missing args";
                return false;
            }

            return true;
        };

        if (name == InstallListenerName()) {
            if (!validateArgs(args, name, 1)) {
                return std::nullopt;
            }

            return InstallListenerMessage{
                .ListenerName = args->GetString(0),
            };
        }
        if (name == RemoveListenerName()) {
            if (!validateArgs(args, name, 1)) {
                return std::nullopt;
            }

            return RemoveListenerMessage{
                .ListenerName = args->GetString(0),
            };
        }
        if (name == InvokeRequestName()) {
            if (!validateArgs(args, name, 2)) {
                return std::nullopt;
            }

            return InvokeRequestMessage{
                .RequestId = *CefUtils::GetValueFromCefList<std::uint64_t>(args, 0),
                .Script = args->GetString(1),
            };
        }
        if (name == InteropCallName()) {
            if (!validateArgs(args, name, 2)) {
                return std::nullopt;
            }

            return InteropCallMessage{
                .FunctionName = args->GetString(0),
                .Argument = args->GetString(1),
            };
        }

        LOG(ERROR) << "ParseBrowserToRendererMessage: " << name << " message name not found";
        return std::nullopt;
    }
}
