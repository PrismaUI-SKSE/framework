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
}

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
}

namespace PrismaUI::ViewManager {
    using namespace ultralight;

    Core::PrismaViewId Create(const std::string& htmlPath,
                              std::move_only_function<void(Core::PrismaViewId)> onDomReadyCallback = nullptr);
    void Show(const Core::PrismaViewId& viewId);
    void Hide(const Core::PrismaViewId& viewId);
    bool IsHidden(const Core::PrismaViewId& viewId);
    bool Focus(const Core::PrismaViewId& viewId, bool pauseGame = false, bool disableFocusMenu = false);
    void Unfocus(const Core::PrismaViewId& viewId);
    bool HasFocus(const Core::PrismaViewId& viewId);
    bool ViewHasInputFocus(const Core::PrismaViewId& viewId);
    void Destroy(const Core::PrismaViewId& viewId);
    bool IsValid(const Core::PrismaViewId& viewId);
    void SetScrollingPixelSize(const Core::PrismaViewId& viewId, int pixelSize);
    int GetScrollingPixelSize(const Core::PrismaViewId& viewId);
    void SetOrder(const Core::PrismaViewId& viewId, int order);
    int GetOrder(const Core::PrismaViewId& viewId);

    bool HasAnyActiveFocus();

    // Console message callback registration
    void RegisterConsoleCallback(
        const Core::PrismaViewId& viewId,
        std::move_only_function<void(Core::PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> callback);
}
