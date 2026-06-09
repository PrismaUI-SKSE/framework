#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include "CefUtils.h"
#include "Messaging.h"
#include "Utils/VariantUtils.h"
#include "cef_process_message.h"
#include "cef_values.h"
#include "include/internal/cef_string.h"

// Stable IPC message names sent from the CEF renderer subprocess
// (PrismaUICefSubprocess.exe) to the browser process (PrismaUI.dll). These
// strings MUST NOT change without updating both sides.

namespace PrismaUI::Cef::RTBMessages {
    struct InvokeResultMessage {
        CefString Result;
        std::uint64_t RequestId = 0;
        bool Success = false;

        static const CefString& GetMessageName() {
            static const CefString value("prisma.invokeResult");
            return value;
        }
    };

    inline void SerializeToList(const InvokeResultMessage& message, const CefRefPtr<CefListValue>& list) {
        CefUtils::SerializeToCefList(list, message.RequestId, message.Success, message.Result);
    }

    inline void DeserializeFromList(const CefRefPtr<CefListValue>& list,
                                    CefUtils::DeserializeResult<InvokeResultMessage>& deserializeResult) {
        Messaging::DeserializeHelper<std::uint64_t, bool, CefString>(list, deserializeResult,
                                                                     [](auto& deserializeResult, auto&& result) {
                                                                         deserializeResult = InvokeResultMessage{
                                                                             .Result = std::move(std::get<2>(result)),
                                                                             .RequestId = std::get<0>(result),
                                                                             .Success = std::get<1>(result),
                                                                         };
                                                                     });
    }

    struct ListenerInvokeMessage {
        CefString ListenerName;
        CefString Argument;

        static const CefString& GetMessageName() {
            static const CefString value("prisma.listenerInvoke");
            return value;
        }
    };

    inline void SerializeToList(const ListenerInvokeMessage& message, const CefRefPtr<CefListValue>& list) {
        CefUtils::SerializeToCefList(list, message.ListenerName, message.Argument);
    }

    inline void DeserializeFromList(const CefRefPtr<CefListValue>& list,
                                    CefUtils::DeserializeResult<ListenerInvokeMessage>& deserializeResult) {
        Messaging::DeserializeHelper<CefString, CefString>(list, deserializeResult,
                                                           [](auto& deserializeResult, auto&& result) {
                                                               deserializeResult = ListenerInvokeMessage{
                                                                   .ListenerName = std::get<0>(result),
                                                                   .Argument = std::get<1>(result),
                                                               };
                                                           });
    }

    struct ConsoleMessage {
        CefString Level;
        CefString Text;

        static const CefString& GetMessageName() {
            static const CefString value("prisma.consoleMessage");
            return value;
        }
    };

    inline void SerializeToList(const ConsoleMessage& message, const CefRefPtr<CefListValue>& list) {
        CefUtils::SerializeToCefList(list, message.Level, message.Text);
    }

    inline void DeserializeFromList(const CefRefPtr<CefListValue>& list,
                                    CefUtils::DeserializeResult<ConsoleMessage>& deserializeResult) {
        Messaging::DeserializeHelper<CefString, CefString>(list, deserializeResult,
                                                           [](auto& deserializeResult, auto&& result) {
                                                               deserializeResult = ConsoleMessage{
                                                                   .Level = std::get<0>(result),
                                                                   .Text = std::get<1>(result),
                                                               };
                                                           });
    }

    struct DomReadyMessage {
        static const CefString& GetMessageName() {
            static const CefString value("prisma.domReady");
            return value;
        }
    };

    inline void SerializeToList(const DomReadyMessage&, const CefRefPtr<CefListValue>&) {}

    inline void DeserializeFromList(const CefRefPtr<CefListValue>&,
                                    CefUtils::DeserializeResult<DomReadyMessage>& deserializeResult) {
        deserializeResult = DomReadyMessage{};
    }

    using RendererToBrowserMessage =
        std::variant<InvokeResultMessage, ListenerInvokeMessage, ConsoleMessage, DomReadyMessage>;
}
