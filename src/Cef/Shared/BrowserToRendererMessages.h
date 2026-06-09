#pragma once
#include <cstdint>
#include <variant>

#include "CefUtils.h"
#include "Messaging.h"
#include "cef_process_message.h"
#include "cef_values.h"
#include "include/internal/cef_string.h"

// Stable IPC message names sent from the browser process (PrismaUI.dll)
// to the CEF renderer subprocess (PrismaUICefSubprocess.exe). These strings
// MUST NOT change without updating both sides.

namespace PrismaUI::Cef::Messages {
    struct InstallListenerMessage {
        CefString ListenerName;

        static const CefString& GetMessageName() {
            static const CefString value("prisma.installListener");
            return value;
        }
    };

    inline void SerializeToList(const InstallListenerMessage& message, const CefRefPtr<CefListValue>& list) {
        CefUtils::SerializeToCefList(list, message.ListenerName);
    }

    inline void DeserializeFromList(const CefRefPtr<CefListValue>& list,
                                    CefUtils::DeserializeResult<InstallListenerMessage>& deserializeResult) {
        Messaging::DeserializeHelper<CefString>(list, deserializeResult, [](auto& deserializeResult, auto&& result) {
            deserializeResult = InstallListenerMessage{
                .ListenerName = std::get<0>(result),
            };
        });
    }

    struct RemoveListenerMessage {
        CefString ListenerName;

        static const CefString& GetMessageName() {
            static const CefString value("prisma.removeListener");
            return value;
        }
    };

    inline void SerializeToList(const RemoveListenerMessage& message, const CefRefPtr<CefListValue>& list) {
        CefUtils::SerializeToCefList(list, message.ListenerName);
    }

    inline void DeserializeFromList(const CefRefPtr<CefListValue>& list,
                                    CefUtils::DeserializeResult<RemoveListenerMessage>& deserializeResult) {
        Messaging::DeserializeHelper<CefString>(list, deserializeResult, [](auto& deserializeResult, auto&& result) {
            deserializeResult = RemoveListenerMessage{
                .ListenerName = std::get<0>(result),
            };
        });
    }

    struct InvokeRequestMessage {
        std::uint64_t RequestId = 0;
        CefString Script;

        static const CefString& GetMessageName() {
            static const CefString value("prisma.invokeRequest");
            return value;
        }
    };

    inline void SerializeToList(const InvokeRequestMessage& message, const CefRefPtr<CefListValue>& list) {
        CefUtils::SerializeToCefList(list, message.RequestId, message.Script);
    }

    inline void DeserializeFromList(const CefRefPtr<CefListValue>& list,
                                    CefUtils::DeserializeResult<InvokeRequestMessage>& deserializeResult) {
        Messaging::DeserializeHelper<std::uint64_t, CefString>(list, deserializeResult,
                                                               [](auto& deserializeResult, auto&& result) {
                                                                   deserializeResult = InvokeRequestMessage{
                                                                       .RequestId = std::get<0>(result),
                                                                       .Script = std::get<1>(result),
                                                                   };
                                                               });
    }

    struct InteropCallMessage {
        CefString FunctionName;
        CefString Argument;

        static const CefString& GetMessageName() {
            static const CefString value("prisma.interopCall");
            return value;
        }
    };

    inline void SerializeToList(const InteropCallMessage& message, const CefRefPtr<CefListValue>& list) {
        CefUtils::SerializeToCefList(list, message.FunctionName, message.Argument);
    }

    inline void DeserializeFromList(const CefRefPtr<CefListValue>& list,
                                    CefUtils::DeserializeResult<InteropCallMessage>& deserializeResult) {
        Messaging::DeserializeHelper<CefString, CefString>(list, deserializeResult,
                                                           [](auto& deserializeResult, auto&& result) {
                                                               deserializeResult = InteropCallMessage{
                                                                   .FunctionName = std::get<0>(result),
                                                                   .Argument = std::get<1>(result),
                                                               };
                                                           });
    }

    using BrowserToRendererMessage =
        std::variant<InstallListenerMessage, RemoveListenerMessage, InvokeRequestMessage, InteropCallMessage>;

}
