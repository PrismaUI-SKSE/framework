#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace PRISMA_UI_API {
    enum class ConsoleMessageLevel : uint8_t;
}

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
}

namespace PrismaUI::ViewManager {
    Core::PrismaViewId Create(const std::string& htmlPath,
                              std::function<void(Core::PrismaViewId)> onDomReadyCallback = nullptr);
    void Show(Core::PrismaViewId viewId);
    void Hide(Core::PrismaViewId viewId);
    bool IsHidden(Core::PrismaViewId viewId);
    bool Focus(Core::PrismaViewId viewId, bool pauseGame = false, bool disableFocusMenu = false);
    void Unfocus(Core::PrismaViewId viewId);
    bool HasFocus(Core::PrismaViewId viewId);
    bool ViewHasInputFocus(Core::PrismaViewId viewId);
    void Destroy(Core::PrismaViewId viewId);
    bool IsValid(Core::PrismaViewId viewId);
    void SetScrollingPixelSize(Core::PrismaViewId viewId, int pixelSize);
    int GetScrollingPixelSize(Core::PrismaViewId viewId);
    void SetOrder(Core::PrismaViewId viewId, int order);
    int GetOrder(Core::PrismaViewId viewId);

    bool HasAnyActiveFocus();

    // Console message callback registration
    void RegisterConsoleCallback(
        Core::PrismaViewId viewId,
        std::function<void(Core::PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> callback);
}
