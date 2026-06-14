#include "Core.h"

#include <vector>

#include "Cef/Browser/CefRuntime.h"
#include "InputHandler.h"
#include "ViewManager.h"

namespace PrismaUI::Core {
    using namespace PrismaUI::ViewManager;

    std::atomic_uint64_t nextViewId = {1};

    std::map<PrismaViewId, std::shared_ptr<PrismaView>> views;
    std::shared_mutex viewsMutex;

    std::map<std::pair<PrismaViewId, std::string>, JSCallbackData> PrismaUI::Core::jsCallbacks;
    std::mutex PrismaUI::Core::jsCallbacksMutex;
}  // namespace PrismaUI::Core
