#pragma once

#include "CefUtils.h"
#include "include/cef_frame.h"
#include "include/cef_process_message.h"

namespace PrismaUI::Cef::Messaging {
    template <class T>
    using MessageValue = std::remove_cvref_t<T>;

    template <class T>
    concept SerializableMessage = requires(const MessageValue<T>& message) {
        { ConvertToProcessMessage(message) } -> std::same_as<CefRefPtr<CefProcessMessage>>;
    };

    template <class T>
    concept ParsableMessage = requires(const CefRefPtr<CefProcessMessage>& processMessage) {
        {
            ParseProcessMessage(std::type_identity<MessageValue<T>>{}, processMessage)
        } -> std::same_as<CefUtils::ParseResult<MessageValue<T>>>;
    };

    template <class T>
    concept Message = SerializableMessage<T> && ParsableMessage<T>;

    template <Message TMessage>
    inline void SendProcessMessageToFrame(const CefRefPtr<CefFrame>& frame, CefProcessId processId,
                                          TMessage&& message) {
        if (!frame) {
            LOG(WARNING) << "SendProcessMessageToFrame: frame is null";
            return;
        }

        CefRefPtr<CefProcessMessage> processMessage = ConvertToProcessMessage(std::forward<TMessage>(message));
        if (!processMessage) {
            LOG(WARNING) << "SendProcessMessageToFrame: failed to convert message to process message";
            return;
        }

        if (!frame->SendProcessMessage(processId, processMessage)) {
            LOG(WARNING) << "SendProcessMessageToFrame: failed to send process message to frame";
        }
    }
}