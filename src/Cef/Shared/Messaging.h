#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <type_traits>
#include <variant>

#include "CefUtils.h"
#include "Utils/VariantUtils.h"
#include "include/cef_frame.h"
#include "include/cef_process_message.h"

namespace PrismaUI::Cef::Messaging {
    template <class T>
    using MessageValue = std::remove_cvref_t<T>;

    template <class T>
    concept Message = requires(const MessageValue<T>& message) {
        { SerializeToList(message, std::declval<const CefRefPtr<CefListValue>&>()) } -> std::same_as<void>;
        {
            DeserializeFromList(std::declval<const CefRefPtr<CefListValue>&>(),
                                std::declval<CefUtils::DeserializeResult<MessageValue<T>>&>())
        } -> std::same_as<void>;
        { MessageValue<T>::GetMessageName() } -> std::same_as<const CefString&>;
    };

    template <Message TMessage>
    void SendProcessMessageToFrame(const CefRefPtr<CefFrame>& frame, CefProcessId processId, TMessage&& message) {
        if (!frame) {
            LOG(WARNING) << "SendProcessMessageToFrame: frame is null";
            return;
        }

        CefRefPtr<CefProcessMessage> processMessage =
            CefProcessMessage::Create(MessageValue<TMessage>::GetMessageName());
        SerializeToList(std::forward<TMessage>(message), processMessage->GetArgumentList());

        frame->SendProcessMessage(processId, processMessage);
    }

    struct NullInput {};

    struct IncorrectMessageName {};

    using DeserializeMessageError = std::variant<NullInput, IncorrectMessageName, CefUtils::DeserializeError>;

    template <class T>
    using DeserializeMessageResult = std::variant<T, DeserializeMessageError>;

    template <Message... TMessages>
    DeserializeMessageResult<std::variant<TMessages...>> DeserializeProcessMessage(
        const CefRefPtr<CefProcessMessage>& message) {
        if (!message) {
            LOG(WARNING) << "DeserializeProcessMessage: message is null";
            return NullInput{};
        }

        const CefString name = message->GetName();
        CefRefPtr<CefListValue> args = message->GetArgumentList();

        std::optional<std::variant<TMessages...>> result;
        std::optional<CefUtils::DeserializeError> error;

        (
            [&] {
                if (result.has_value() || error.has_value()) {
                    return;
                }

                if (name == TMessages::GetMessageName()) {
                    CefUtils::DeserializeResult<TMessages> deserializeResult;
                    DeserializeFromList(args, deserializeResult);
                    Match(
                        deserializeResult, [&](const TMessages& m) { result.emplace(std::move(m)); },
                        [&](const CefUtils::DeserializeError& e) { error.emplace(std::move(e)); });
                }
            }(),
            ...);

        if (result.has_value()) {
            return *result;
        }

        if (error.has_value()) {
            LOG(WARNING) << "DeserializeProcessMessage: deserialization error for " << name;
            return *error;
        }

        LOG(WARNING) << "DeserializeProcessMessage: incorrect message name: " << name;
        return IncorrectMessageName{};
    }

    namespace detail {
        template <class T>
        struct ProcessMessageVariantDeserializer;

        template <Message... TMessages>
        struct ProcessMessageVariantDeserializer<std::variant<TMessages...>> {
            static DeserializeMessageResult<std::variant<TMessages...>> Deserialize(
                const CefRefPtr<CefProcessMessage>& message) {
                return DeserializeProcessMessage<TMessages...>(message);
            }
        };
    }

    template <class TVariant>
    DeserializeMessageResult<MessageValue<TVariant>> DeserializeProcessMessageVariant(
        const CefRefPtr<CefProcessMessage>& message) {
        return detail::ProcessMessageVariantDeserializer<MessageValue<TVariant>>::Deserialize(message);
    }

    template <class... Args, Message TResult,
              std::invocable<CefUtils::DeserializeResult<TResult>&, std::tuple<Args...>&&> TSuccessHandler>
    void DeserializeHelper(const CefRefPtr<CefListValue>& list, CefUtils::DeserializeResult<TResult>& deserializeResult,
                           TSuccessHandler&& successHandler) {
        Match(
            CefUtils::DeserializeCefList<Args...>(list),
            [&](const std::tuple<Args...>& result) {
                std::invoke(std::forward<TSuccessHandler>(successHandler), deserializeResult, std::move(result));
            },
            [&](const CefUtils::DeserializeError& error) { deserializeResult = error; });
    }
}