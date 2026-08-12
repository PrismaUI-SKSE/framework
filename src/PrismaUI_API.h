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

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

typedef uint64_t PrismaView;

namespace PRISMA_UI_API {
    constexpr const auto PrismaUIPluginName = "PrismaUI";

    // Available PrismaUI interface versions
    enum class InterfaceVersion : uint8_t { V1, V2, V3 };

    typedef void (*OnDomReadyCallback)(PrismaView view);
    typedef void (*JSCallback)(const char* result);
    typedef void (*JSListenerCallback)(const char* argument);

    // JavaScript console message severity level for use with RegisterConsoleCallback().
    enum class ConsoleMessageLevel : uint8_t { Log = 0, Warning, Error, Debug, Info };

    // Console message callback.
    typedef void (*ConsoleMessageCallback)(PrismaView view, ConsoleMessageLevel level, const char* message);

    /// Mouse button values accepted by an external surface host.
    enum class PointerButton : uint8_t { Left = 0, Middle, Right };

    /// Strong references to the current D3D11 texture exposed to an external host.
    struct ExternalSurface {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* shaderResourceView = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t generation = 0;
    };

    /// Snapshot of a live PrismaUI view returned by view enumeration.
    struct ViewDescriptor {
        PrismaView view = 0;
        char htmlPath[512]{};
        uint8_t hidden = 0;
        uint8_t externalSurfaceHost = 0;
    };

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

    // PrismaUI modder interface v3 (extends v2)
    class IVPrismaUI3 : public IVPrismaUI2 {
    protected:
        ~IVPrismaUI3() = default;

    public:
        /// Enables or disables external presentation while keeping the view rendered.
        /// @return True when the view exists and the hosting state was changed.
        virtual bool SetExternalSurfaceHost(PrismaView view, bool enabled) noexcept = 0;

        /// Acquires strong COM references to the current D3D11 surface.
        /// Release them with ReleaseSurface before acquiring another surface.
        /// @return True when a render surface is currently available.
        virtual bool AcquireSurface(PrismaView view, ExternalSurface* surface) noexcept = 0;

        /// Releases COM references previously returned by AcquireSurface.
        virtual void ReleaseSurface(ExternalSurface* surface) noexcept = 0;

        /// Injects pointer movement in view pixel coordinates.
        /// @return False unless external surface hosting is enabled for the view.
        virtual bool SendPointerMove(PrismaView view, int32_t x, int32_t y) noexcept = 0;

        /// Injects a pointer button transition in view pixel coordinates.
        /// @return False unless external surface hosting is enabled for the view.
        virtual bool SendPointerButton(PrismaView view, int32_t x, int32_t y, PointerButton button,
                                       bool pressed) noexcept = 0;

        /// Injects a pixel-based pointer scroll delta.
        /// @return False unless external surface hosting is enabled for the view.
        virtual bool SendPointerScroll(PrismaView view, int32_t deltaX, int32_t deltaY) noexcept = 0;

        /// Returns the total view count and fills up to capacity entries when output is non-null.
        uint32_t EnumerateViews(ViewDescriptor* output, uint32_t capacity) noexcept {
            using EnumerateViewsFunc = uint32_t (*)(ViewDescriptor*, uint32_t) noexcept;
            const auto pluginHandle = GetModuleHandleW(L"PrismaUI.dll");
            if (!pluginHandle) return 0;
            const auto enumerateViews = reinterpret_cast<EnumerateViewsFunc>(
                GetProcAddress(pluginHandle, "PrismaUI_EnumerateViews"));
            return enumerateViews ? enumerateViews(output, capacity) : 0;
        }
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

    template <>
    struct InterfaceVersionMap<IVPrismaUI3> {
        static constexpr InterfaceVersion version = InterfaceVersion::V3;
    };

    typedef void* (*RequestPluginAPIFunc)(InterfaceVersion interfaceVersion);

    /// Request the PrismaUI API interface.
    /// Recommended: Send your request during or after SKSEMessagingInterface::kMessage_PostLoad to make sure the dll
    /// has already been loaded
    [[nodiscard]] inline void* RequestPluginAPI(InterfaceVersion a_interfaceVersion = InterfaceVersion::V1) {
        auto pluginHandle = GetModuleHandleW(L"PrismaUI.dll");
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
    ///   auto* m_prismaUIv3 = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI3>();
    template <typename T>
    [[nodiscard]] inline T* RequestPluginAPI() {
        return static_cast<T*>(RequestPluginAPI(InterfaceVersionMap<T>::version));
    }
}
