#pragma once

#include <cstdint>
#include <functional>

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/StringSTL.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

namespace PRISMA_UI_API {
    enum class ConsoleMessageLevel : uint8_t;
    enum class PointerButton : uint8_t;
    struct ExternalSurface;
    struct ViewDescriptor;
}

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
}

namespace PrismaUI::ViewManager {
    using namespace ultralight;

    /// Creates a view while safely requesting deferred core initialization.
    Core::PrismaViewId Create(const std::string& htmlPath,
                              std::function<void(Core::PrismaViewId)> onDomReadyCallback = nullptr);
    void Show(const Core::PrismaViewId& viewId);
    /// Hides a view immediately and queues its focus cleanup.
    void Hide(const Core::PrismaViewId& viewId);
    bool IsHidden(const Core::PrismaViewId& viewId);
    bool Focus(const Core::PrismaViewId& viewId, bool pauseGame = false, bool disableFocusMenu = false);
    void Unfocus(const Core::PrismaViewId& viewId);
    bool HasFocus(const Core::PrismaViewId& viewId);
    bool ViewHasInputFocus(const Core::PrismaViewId& viewId);
    /// Destroys a view and releases all regular and external D3D11 resources.
    void Destroy(const Core::PrismaViewId& viewId);
    bool IsValid(const Core::PrismaViewId& viewId);
    void SetScrollingPixelSize(const Core::PrismaViewId& viewId, int pixelSize);
    int GetScrollingPixelSize(const Core::PrismaViewId& viewId);
    void SetOrder(const Core::PrismaViewId& viewId, int order);
    int GetOrder(const Core::PrismaViewId& viewId);

    // Inspector View functions
    void CreateInspectorView(const Core::PrismaViewId& viewId);
    void SetInspectorVisibility(const Core::PrismaViewId& viewId, bool visible);
    bool IsInspectorVisible(const Core::PrismaViewId& viewId);
    void SetInspectorBounds(const Core::PrismaViewId& viewId, float topLeftX, float topLeftY, uint32_t width,
                            uint32_t height);
    bool HasAnyActiveFocus();

    // Console message callback registration
    void RegisterConsoleCallback(const Core::PrismaViewId& viewId,
                                 std::function<void(Core::PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> callback);

    /// Changes whether PrismaUI or an external consumer presents the view.
    bool SetExternalSurfaceHost(const Core::PrismaViewId& viewId, bool enabled);
    /// Acquires the current view surface with strong COM references.
    bool AcquireSurface(const Core::PrismaViewId& viewId, PRISMA_UI_API::ExternalSurface* surface);
    /// Releases references acquired through AcquireSurface.
    void ReleaseSurface(PRISMA_UI_API::ExternalSurface* surface);
    /// Sends pointer movement directly to an externally hosted view.
    bool SendPointerMove(const Core::PrismaViewId& viewId, int32_t x, int32_t y);
    /// Sends a pointer button transition directly to an externally hosted view.
    bool SendPointerButton(const Core::PrismaViewId& viewId, int32_t x, int32_t y,
                           PRISMA_UI_API::PointerButton button, bool pressed);
    /// Sends a pointer scroll delta directly to an externally hosted view.
    bool SendPointerScroll(const Core::PrismaViewId& viewId, int32_t deltaX, int32_t deltaY);
    /// Enumerates live views into a caller-owned descriptor buffer.
    uint32_t EnumerateViews(PRISMA_UI_API::ViewDescriptor* output, uint32_t capacity);
}
