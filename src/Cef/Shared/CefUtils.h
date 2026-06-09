#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <source_location>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "include/cef_process_message.h"
#include "include/cef_v8.h"
#include "include/cef_values.h"
#include "internal/cef_string.h"

namespace PrismaUI::Cef::CefUtils {
    struct CefStringHash {
        std::size_t operator()(const CefString& s) const noexcept {
            using Char = CefString::char_type;
            using View = std::basic_string_view<Char>;

            return std::hash<View>{}(View{s.c_str(), s.length()});
        }
    };

    inline bool ExecuteFunction(const CefRefPtr<CefV8Value>& function, const CefV8ValueList& args,
                                const std::source_location location = std::source_location::current()) {
        if (!function) {
            LOG(ERROR) << "Execute V8 function: is not a valid pointer. Called by function: "
                       << location.function_name();
            return false;
        }

        if (!function->IsFunction()) {
            LOG(ERROR) << "Execute V8 function: is not a function. Called by function: " << location.function_name();
            return false;
        }

        function->ExecuteFunction(nullptr, args);
        if (function->HasException()) {
            CefRefPtr<CefV8Exception> ex = function->GetException();
            LOG(ERROR) << "Execute V8 function '" << function->GetFunctionName()
                       << "' threw: " << (ex ? ex->GetMessage() : CefString{})
                       << ". Called by function: " << location.function_name();
            return false;
        }

        return true;
    }

    template <std::invocable<const CefRefPtr<CefV8Context>&> T>
    void EnterContext(const CefRefPtr<CefV8Context>& context, T&& func) {
        if (!context) {
            return;
        }

        if (!context->Enter()) {
            LOG(ERROR) << "Failed to enter V8 context";
            return;
        }

        std::invoke(std::forward<T>(func), context);

        if (!context->Exit()) {
            LOG(ERROR) << "Failed to exit V8 context";
        }
    }

    template <class... Args>
    void SerializeToCefList(const CefRefPtr<CefListValue>& list, const Args&... args) {
        if (!list) {
            return;
        }

        list->SetSize(sizeof...(Args));
        size_t i = 0;
        (
            [&] {
                using T = std::decay_t<Args>;
                const size_t index = i++;
                if constexpr (std::is_convertible_v<T, CefString>) {
                    list->SetString(index, args);
                } else if constexpr (std::is_same_v<T, bool>) {
                    list->SetBool(index, args);
                } else if constexpr (std::is_integral_v<T>) {
                    if constexpr (sizeof(T) <= sizeof(int)) {
                        list->SetInt(index, static_cast<int>(args));
                    } else {
                        list->SetDouble(index, static_cast<double>(args));
                    }
                } else if constexpr (std::is_floating_point_v<T>) {
                    list->SetDouble(index, static_cast<double>(args));
                } else {
                    static_assert(!sizeof(T), "Unsupported type");
                }
            }(),
            ...);
    }

    template <class... Args>
    CefRefPtr<CefProcessMessage> MakeListMessage(const CefString& messageName, const Args&... args) {
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(messageName);
        SerializeToCefList(msg->GetArgumentList(), args...);

        return msg;
    }

    template <class T>
    std::optional<T> GetValueFromCefList(const CefRefPtr<CefListValue>& list, size_t index) {
        if (!list || list->GetSize() <= index) {
            return std::nullopt;
        }

        if constexpr (std::is_convertible_v<CefString, T>) {
            if (list->GetType(index) != VTYPE_STRING) {
                return std::nullopt;
            }

            return list->GetString(index);
        } else if constexpr (std::is_same_v<T, bool>) {
            if (list->GetType(index) != VTYPE_BOOL) {
                return std::nullopt;
            }

            return list->GetBool(index);
        } else if constexpr (std::is_integral_v<T>) {
            if constexpr (sizeof(T) <= sizeof(int)) {
                if (list->GetType(index) != VTYPE_INT) {
                    return std::nullopt;
                }

                return static_cast<T>(list->GetInt(index));
            } else {
                if (list->GetType(index) != VTYPE_DOUBLE) {
                    return std::nullopt;
                }

                return static_cast<T>(list->GetDouble(index));
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            if (list->GetType(index) != VTYPE_DOUBLE) {
                return std::nullopt;
            }

            return static_cast<T>(list->GetDouble(index));
        } else {
            static_assert(!sizeof(T), "Unsupported type");
            return T{};
        }
    }

    struct InvalidArgCount {
        int Expected;
    };

    struct InvalidArg {
        int Index;
    };

    using DeserializeError = std::variant<InvalidArgCount, InvalidArg>;

    template <class T>
    using DeserializeResult = std::variant<T, DeserializeError>;

    template <class... Args>
    DeserializeResult<std::tuple<Args...>> DeserializeCefList(const CefRefPtr<CefListValue>& list) {
        if (!list || list->GetSize() < sizeof...(Args)) {
            LOG(WARNING) << "DeserializeCefList: list is null or has less args than expected. Expected: "
                         << sizeof...(Args) << ", got: " << (list ? std::to_string(list->GetSize()) : "null");
            return InvalidArgCount{sizeof...(Args)};
        }

        std::tuple<Args...> result;
        std::optional<DeserializeError> error;

        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (
                [&] {
                    using T = std::tuple_element_t<I, std::tuple<Args...>>;

                    if (error.has_value()) {
                        return;
                    }

                    auto value = GetValueFromCefList<T>(list, I);
                    if (!value.has_value()) {
                        LOG(WARNING) << "DeserializeCefList: error deserializing list at index: " << I;
                        error.emplace(InvalidArg{static_cast<int>(I)});
                        return;
                    }

                    std::get<I>(result) = std::move(*value);
                }(),
                ...);
        }(std::index_sequence_for<Args...>{});

        return !error.has_value() ? DeserializeResult<std::tuple<Args...>>(std::move(result)) : error.value();
    }
}