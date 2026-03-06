/*
 * For modders: Copy this file into your own project if you wish to use this API.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include <Windows.h>
#include <stdint.h>

typedef uint64_t PrismaView;

namespace PRISMA_UI_API {
    constexpr const auto PrismaUIPluginName = "PrismaUI";

    // Available PrismaUI interface versions
    enum class InterfaceVersion : uint8_t { V1, V2 };

    typedef void (*OnDomReadyCallback)(PrismaView view);
    typedef void (*JSCallback)(const char* result);
    typedef void (*JSListenerCallback)(const char* argument);

    // JavaScript console message severity level for use with RegisterConsoleCallback().
    enum class ConsoleMessageLevel : uint8_t { Log = 0, Warning, Error, Debug, Info };

    // Console message callback.
    typedef void (*ConsoleMessageCallback)(PrismaView view, ConsoleMessageLevel level, const char* message);

    // PrismaUI modder interface v1
    class IVPrismaUI1 {
    protected:
        ~IVPrismaUI1() = default;

    public:
        // Create view.
        virtual PrismaView CreateView(const char* htmlPath,
                                      OnDomReadyCallback onDomReadyCallback = nullptr) noexcept = 0;

        // Send JS code to UI.
        virtual void Invoke(PrismaView view, const char* script, JSCallback callback = nullptr) noexcept = 0;

        // Call JS function through JS Interop API (best performance).
        virtual void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept = 0;

        // Register JS listener.
        virtual void RegisterJSListener(PrismaView view, const char* functionName,
                                        JSListenerCallback callback) noexcept = 0;

        // Returns true if view has focus.
        virtual bool HasFocus(PrismaView view) noexcept = 0;

        // Set focus on view.
        virtual bool Focus(PrismaView view, bool pauseGame = false, bool disableFocusMenu = false) noexcept = 0;

        // Remove focus from view.
        virtual void Unfocus(PrismaView view) noexcept = 0;

        // Show a hidden view.
        virtual void Show(PrismaView view) noexcept = 0;

        // Hide a visible view.
        virtual void Hide(PrismaView view) noexcept = 0;

        // Returns true if view is hidden.
        virtual bool IsHidden(PrismaView view) noexcept = 0;

        // Get scroll size in pixels.
        virtual int GetScrollingPixelSize(PrismaView view) noexcept = 0;

        // Set scroll size in pixels.
        virtual void SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept = 0;

        // Returns true if view exists.
        virtual bool IsValid(PrismaView view) noexcept = 0;

        // Completely destroy view.
        virtual void Destroy(PrismaView view) noexcept = 0;

        // Set view order.
        virtual void SetOrder(PrismaView view, int order) noexcept = 0;

        // Get view order.
        virtual int GetOrder(PrismaView view) noexcept = 0;

        // Create inspector view for debugging.
        virtual void CreateInspectorView(PrismaView view) noexcept = 0;

        // Show or hide the inspector overlay.
        virtual void SetInspectorVisibility(PrismaView view, bool visible) noexcept = 0;

        // Returns true if inspector is visible.
        virtual bool IsInspectorVisible(PrismaView view) noexcept = 0;

        // Set inspector window position and size.
        virtual void SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY, unsigned int width,
                                        unsigned int height) noexcept = 0;

        // Returns true if any view has active focus.
        virtual bool HasAnyActiveFocus() noexcept = 0;
    };

    // PrismaUI modder interface v2 (extends v1)
    class IVPrismaUI2 : public IVPrismaUI1 {
    protected:
        ~IVPrismaUI2() = default;

    public:
        // Register a callback to receive JavaScript console messages from a view.
        // Pass nullptr to unregister.
        virtual void RegisterConsoleCallback(PrismaView view, ConsoleMessageCallback callback) noexcept = 0;
    };

    // Maps interface types to InterfaceVersion enum values.
    // compile-time constraint -- only request interface versions that actually exist.
    template <typename T>
    struct InterfaceVersionMap;

    template <>
    struct InterfaceVersionMap<IVPrismaUI1> {
        static constexpr InterfaceVersion version = InterfaceVersion::V1;
    };

    template <>
    struct InterfaceVersionMap<IVPrismaUI2> {
        static constexpr InterfaceVersion version = InterfaceVersion::V2;
    };

    typedef void* (*RequestPluginAPIFunc)(InterfaceVersion interfaceVersion);

    /// Request the PrismaUI API interface.
    /// Recommended: Send your request during or after SKSEMessagingInterface::kMessage_PostLoad to make sure the dll
    /// has already been loaded
    [[nodiscard]] inline void* RequestPluginAPI(InterfaceVersion a_interfaceVersion = InterfaceVersion::V1) {
        auto pluginHandle = GetModuleHandle(L"PrismaUI.dll");
        if (!pluginHandle) {
            return nullptr;
        }

        auto requestAPIFunction =
            reinterpret_cast<RequestPluginAPIFunc>(GetProcAddress(pluginHandle, "RequestPluginAPI"));

        if (requestAPIFunction) {
            return requestAPIFunction(a_interfaceVersion);
        }

        return nullptr;
    }

    /// Request a specific PrismaUI API interface version.
    /// Returns nullptr if the loaded PrismaUI DLL does not support the requested version.
    ///
    /// Usage:
    ///   auto* m_prismaUI   = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
    ///   auto* m_prismaUIv2 = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI2>();
    template <typename T>
    [[nodiscard]] inline T* RequestPluginAPI() {
        return static_cast<T*>(RequestPluginAPI(InterfaceVersionMap<T>::version));
    }
}
