#pragma once

// Stable IPC message names shared between the browser process (PrismaUI.dll)
// and the CEF renderer subprocess (PrismaUICefSubprocess.exe). These strings
// MUST NOT change without updating both sides.

namespace PrismaUI::Cef::Messages {
    // Browser -> renderer.
    inline constexpr const char* kInstallListener = "prisma.installListener";
    inline constexpr const char* kRemoveListener = "prisma.removeListener";
    inline constexpr const char* kInvokeRequest = "prisma.invokeRequest";
    inline constexpr const char* kInteropCall = "prisma.interopCall";

    // Renderer -> browser.
    inline constexpr const char* kInvokeResult = "prisma.invokeResult";
    inline constexpr const char* kListenerInvoke = "prisma.listenerInvoke";
    inline constexpr const char* kConsoleMessage = "prisma.consoleMessage";
    inline constexpr const char* kDomReady = "prisma.domReady";
    inline constexpr const char* kImeFocusListener = "__prismaNativeImeFocusChanged";

    // Iframe frame-name prefix used to correlate process messages back to a
    // PrismaUI view id ("prisma-view-<id>").
    inline constexpr const char* kIframeNamePrefix = "prisma-view-";
}
