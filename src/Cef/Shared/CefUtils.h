#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <source_location>
#include <string_view>
#include <utility>

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

    // Build a process message with the given (name, [string args]) shape.
    template <class... Args>
    CefRefPtr<CefProcessMessage> MakeListMessage(const CefString& messageName, const Args&... args) {
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(messageName);
        CefRefPtr<CefListValue> list = msg->GetArgumentList();
        list->SetSize(sizeof...(Args));
        size_t i = 0;
        (list->SetString(i++, args), ...);

        return msg;
    }
}