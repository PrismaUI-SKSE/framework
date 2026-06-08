#pragma once

#include <cstdint>
#include <string_view>

#include "include/internal/cef_string.h"

// Stable IPC message names shared between the browser process (PrismaUI.dll)
// and the CEF renderer subprocess (PrismaUICefSubprocess.exe). These strings
// MUST NOT change without updating both sides.

namespace PrismaUI::Cef::Messages {
    namespace Detail {
        inline constexpr const char* kInstallListener = "prisma.installListener";
        inline constexpr const char* kRemoveListener = "prisma.removeListener";
        inline constexpr const char* kInvokeRequest = "prisma.invokeRequest";
        inline constexpr const char* kInteropCall = "prisma.interopCall";

        inline constexpr const char* kInvokeResult = "prisma.invokeResult";
        inline constexpr const char* kListenerInvoke = "prisma.listenerInvoke";
        inline constexpr const char* kConsoleMessage = "prisma.consoleMessage";
        inline constexpr const char* kDomReady = "prisma.domReady";
    }

    // Browser -> renderer.
    inline const CefString& InstallListenerName() {
        static const CefString value(Detail::kInstallListener);
        return value;
    }

    inline const CefString& RemoveListenerName() {
        static const CefString value(Detail::kRemoveListener);
        return value;
    }

    inline const CefString& InvokeRequestName() {
        static const CefString value(Detail::kInvokeRequest);
        return value;
    }

    inline const CefString& InteropCallName() {
        static const CefString value(Detail::kInteropCall);
        return value;
    }

    // Renderer -> browser.
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

    inline constexpr const char* kImeFocusListener = "__prismaNativeImeFocusChanged";

    enum class BrowserToRendererMessage : std::uint8_t {
        Unknown,
        InstallListener,
        RemoveListener,
        InvokeRequest,
        InteropCall
    };

    enum class RendererToBrowserMessage : std::uint8_t {
        Unknown,
        InvokeResult,
        ListenerInvoke,
        ConsoleMessage,
        DomReady
    };

    inline BrowserToRendererMessage ClassifyBrowserToRendererMessage(const CefString& name) {
        if (name == InstallListenerName()) {
            return BrowserToRendererMessage::InstallListener;
        }
        if (name == RemoveListenerName()) {
            return BrowserToRendererMessage::RemoveListener;
        }
        if (name == InvokeRequestName()) {
            return BrowserToRendererMessage::InvokeRequest;
        }
        if (name == InteropCallName()) {
            return BrowserToRendererMessage::InteropCall;
        }
        return BrowserToRendererMessage::Unknown;
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

    inline const CefString& ToCefString(BrowserToRendererMessage message) {
        switch (message) {
            case BrowserToRendererMessage::InstallListener:
                return InstallListenerName();
            case BrowserToRendererMessage::RemoveListener:
                return RemoveListenerName();
            case BrowserToRendererMessage::InvokeRequest:
                return InvokeRequestName();
            case BrowserToRendererMessage::InteropCall:
                return InteropCallName();
            default:
                return InvokeRequestName();
        }
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

    constexpr std::string_view ToStringView(BrowserToRendererMessage message) noexcept {
        switch (message) {
            case BrowserToRendererMessage::InstallListener:
                return Detail::kInstallListener;
            case BrowserToRendererMessage::RemoveListener:
                return Detail::kRemoveListener;
            case BrowserToRendererMessage::InvokeRequest:
                return Detail::kInvokeRequest;
            case BrowserToRendererMessage::InteropCall:
                return Detail::kInteropCall;
            default:
                return "<unknown>";
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

    // Iframe frame-name prefix used to correlate process messages back to a
    // PrismaUI view id ("prisma-view-<id>").
    inline constexpr const char* kIframeNamePrefix = "prisma-view-";
}
