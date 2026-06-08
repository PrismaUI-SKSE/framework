#pragma once

#include <cstdint>
#include <string_view>

#include "include/internal/cef_string.h"

// Stable IPC message names sent from the browser process (PrismaUI.dll)
// to the CEF renderer subprocess (PrismaUICefSubprocess.exe). These strings
// MUST NOT change without updating both sides.

namespace PrismaUI::Cef::Messages {
    namespace Detail {
        inline constexpr const char* kInstallListener = "prisma.installListener";
        inline constexpr const char* kRemoveListener = "prisma.removeListener";
        inline constexpr const char* kInvokeRequest = "prisma.invokeRequest";
        inline constexpr const char* kInteropCall = "prisma.interopCall";
    }

    enum class BrowserToRendererMessage : std::uint8_t {
        Unknown,
        InstallListener,
        RemoveListener,
        InvokeRequest,
        InteropCall
    };

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
}
