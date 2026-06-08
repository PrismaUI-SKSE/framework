#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "CefUtils.h"
#include "cef_process_message.h"
#include "cef_values.h"
#include "include/internal/cef_string.h"

// Stable IPC message names sent from the CEF renderer subprocess
// (PrismaUICefSubprocess.exe) to the browser process (PrismaUI.dll). These
// strings MUST NOT change without updating both sides.

namespace PrismaUI::Cef::Messages {
    inline const CefString& InvokeResultName() {
        static const CefString value("prisma.invokeResult");
        return value;
    }

    inline const CefString& ListenerInvokeName() {
        static const CefString value("prisma.listenerInvoke");
        return value;
    }

    inline const CefString& ConsoleMessageName() {
        static const CefString value("prisma.consoleMessage");
        return value;
    }

    inline const CefString& DomReadyName() {
        static const CefString value("prisma.domReady");
        return value;
    }

    inline CefRefPtr<CefProcessMessage> CreateInvokeResultMessage(std::uint64_t requestId, bool success,
                                                                  const CefString& result) {
        return CefUtils::MakeListMessage(InvokeResultName(), requestId, success, result);
    }

    inline CefRefPtr<CefProcessMessage> CreateListenerInvokeMessage(const CefString& listenerName,
                                                                    const CefString& argument) {
        return CefUtils::MakeListMessage(ListenerInvokeName(), listenerName, argument);
    }

    inline CefRefPtr<CefProcessMessage> CreateConsoleMessage(const CefString& level, const CefString& text) {
        return CefUtils::MakeListMessage(ConsoleMessageName(), level, text);
    }

    inline CefRefPtr<CefProcessMessage> CreateDomReadyMessage() { return CefUtils::MakeListMessage(DomReadyName()); }

    struct InvokeResultMessage {
        CefString Result;
        std::uint64_t RequestId = 0;
        bool Success = false;
    };

    struct ListenerInvokeMessage {
        CefString ListenerName;
        CefString Argument;
    };

    struct ConsoleMessage {
        CefString Level;
        CefString Text;
    };

    struct DomReadyMessage {};

    using RendererToBrowserMessage =
        std::variant<InvokeResultMessage, ListenerInvokeMessage, ConsoleMessage, DomReadyMessage>;

    inline std::optional<RendererToBrowserMessage> ParseRendererToBrowserMessage(
        const CefRefPtr<CefProcessMessage>& message) {
        const CefString name = message->GetName();
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        if (!args) {
            LOG(ERROR) << "ParseRendererToBrowserMessage: " << name << " args pointer is null";
            return std::nullopt;
        }

        auto validateArgs = [](const CefRefPtr<CefListValue>& args, const CefString& name, int requiredCount) {
            if (args->GetSize() < requiredCount) {
                LOG(ERROR) << "ParseRendererToBrowserMessage: " << name << " missing args";
                return false;
            }

            return true;
        };

        if (name == InvokeResultName()) {
            if (!validateArgs(args, name, 3)) {
                return std::nullopt;
            }

            return InvokeResultMessage{
                .Result = args->GetString(2),
                .RequestId = CefUtils::GetValueFromCefList<std::uint64_t>(args, 0),
                .Success = CefUtils::GetValueFromCefList<bool>(args, 1),
            };
        }
        if (name == ListenerInvokeName()) {
            if (!validateArgs(args, name, 2)) {
                return std::nullopt;
            }

            return ListenerInvokeMessage{
                .ListenerName = args->GetString(0),
                .Argument = args->GetString(1),
            };
        }
        if (name == ConsoleMessageName()) {
            if (!validateArgs(args, name, 2)) {
                return std::nullopt;
            }

            return ConsoleMessage{
                .Level = args->GetString(0),
                .Text = args->GetString(1),
            };
        }
        if (name == DomReadyName()) {
            return DomReadyMessage{};
        }

        LOG(ERROR) << "ParseRendererToBrowserMessage: " << name << " message name not found";
        return std::nullopt;
    }
}
