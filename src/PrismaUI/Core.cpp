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

    void Shutdown() {
        logger::info("Shutting down PrismaUI Core System...");

        std::vector<PrismaViewId> viewIdsToDestroy;
        {
            std::shared_lock lock(viewsMutex);
            for (const auto& pair : views) {
                viewIdsToDestroy.push_back(pair.first);
                if (pair.second) {
                    // Mark each view as destroyRequested so any in-flight queue entries
                    // observe it and no-op before reaching CEF or render state (Step 6).
                    pair.second->destroyRequested.store(true, std::memory_order_release);
                }
            }
        }

        for (const auto& id : viewIdsToDestroy) {
            try {
                ViewManager::Destroy(id);
            } catch (const std::exception& e) {
                logger::error("Error destroying view [{}] during shutdown: {}", id, e.what());
            }
        }

        {
            std::unique_lock lock(viewsMutex);
            views.clear();
        }
        logger::info("PrismaUI Core System shut down complete.");
    }
}  // namespace PrismaUI::Core
