#pragma once

#include <cstdint>
#include <string_view>

#include "include/internal/cef_string.h"

// Stable IPC message names sent from the CEF renderer subprocess
// (PrismaUICefSubprocess.exe) to the browser process (PrismaUI.dll). These
// strings MUST NOT change without updating both sides.

namespace PrismaUI::Cef::Messages {
    namespace Detail {
        inline constexpr const char* kInvokeResult = "prisma.invokeResult";
        inline constexpr const char* kListenerInvoke = "prisma.listenerInvoke";
        inline constexpr const char* kConsoleMessage = "prisma.consoleMessage";
        inline constexpr const char* kDomReady = "prisma.domReady";
    }

    enum class RendererToBrowserMessage : std::uint8_t {
        Unknown,
        InvokeResult,
        ListenerInvoke,
        ConsoleMessage,
        DomReady
    };

    inline const CefString& InvokeResultName() {
        static const CefString value(Detail::kInvokeResult);
        return value;
    }

    inline const CefString& ListenerInvokeName() {
        static const CefString value(Detail::kListenerInvoke);
        return value;
    }

    inline const CefString& ConsoleMessageName() {
        static const CefString value(Detail::kConsoleMessage);
        return value;
    }

    inline const CefString& DomReadyName() {
        static const CefString value(Detail::kDomReady);
        return value;
    }

    inline RendererToBrowserMessage ClassifyRendererToBrowserMessage(const CefString& name) {
        if (name == InvokeResultName()) {
            return RendererToBrowserMessage::InvokeResult;
        }
        if (name == ListenerInvokeName()) {
            return RendererToBrowserMessage::ListenerInvoke;
        }
        if (name == ConsoleMessageName()) {
            return RendererToBrowserMessage::ConsoleMessage;
        }
        if (name == DomReadyName()) {
            return RendererToBrowserMessage::DomReady;
        }
        return RendererToBrowserMessage::Unknown;
    }

    inline const CefString& ToCefString(RendererToBrowserMessage message) {
        switch (message) {
            case RendererToBrowserMessage::InvokeResult:
                return InvokeResultName();
            case RendererToBrowserMessage::ListenerInvoke:
                return ListenerInvokeName();
            case RendererToBrowserMessage::ConsoleMessage:
                return ConsoleMessageName();
            case RendererToBrowserMessage::DomReady:
                return DomReadyName();
            default:
                return DomReadyName();
        }
    }

    constexpr std::string_view ToStringView(RendererToBrowserMessage message) noexcept {
        switch (message) {
            case RendererToBrowserMessage::InvokeResult:
                return Detail::kInvokeResult;
            case RendererToBrowserMessage::ListenerInvoke:
                return Detail::kListenerInvoke;
            case RendererToBrowserMessage::ConsoleMessage:
                return Detail::kConsoleMessage;
            case RendererToBrowserMessage::DomReady:
                return Detail::kDomReady;
            default:
                return "<unknown>";
        }
    }
}
