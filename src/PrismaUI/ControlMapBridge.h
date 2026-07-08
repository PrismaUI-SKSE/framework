#pragma once

#include <cstdint>
#include <string>

namespace PrismaUI::Core {
    typedef uint64_t PrismaViewId;
}

// Exposes RE::ControlMap (the game's keybinding registry) to a view's JavaScript
namespace PrismaUI::ControlMapBridge {
    // Queues a view to receive a control snapshot
    void RequestRefresh(Core::PrismaViewId viewId);

    // Drains queued refresh requests and sends the updated control map to each view's window.prismaUi object
    void ProcessPendingRefreshes();
}
