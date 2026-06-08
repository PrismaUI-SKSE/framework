#pragma once

#include <cstdint>
#include <string_view>

#include "include/internal/cef_string.h"

// Stable IPC message names shared between the browser process (PrismaUI.dll)
// and the CEF renderer subprocess (PrismaUICefSubprocess.exe). These strings
// MUST NOT change without updating both sides.

namespace PrismaUI::Cef::Messages {
    // Browser -> renderer.
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

    // Renderer -> browser.
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
                return "prisma.installListener";
            case BrowserToRendererMessage::RemoveListener:
                return "prisma.removeListener";
            case BrowserToRendererMessage::InvokeRequest:
                return "prisma.invokeRequest";
            case BrowserToRendererMessage::InteropCall:
                return "prisma.interopCall";
            default:
                return "<unknown>";
        }
    }

    constexpr std::string_view ToStringView(RendererToBrowserMessage message) noexcept {
        switch (message) {
            case RendererToBrowserMessage::InvokeResult:
                return "prisma.invokeResult";
            case RendererToBrowserMessage::ListenerInvoke:
                return "prisma.listenerInvoke";
            case RendererToBrowserMessage::ConsoleMessage:
                return "prisma.consoleMessage";
            case RendererToBrowserMessage::DomReady:
                return "prisma.domReady";
            default:
                return "<unknown>";
        }
    }

    // Iframe frame-name prefix used to correlate process messages back to a
    // PrismaUI view id ("prisma-view-<id>").
    inline constexpr const char* kIframeNamePrefix = "prisma-view-";
}
