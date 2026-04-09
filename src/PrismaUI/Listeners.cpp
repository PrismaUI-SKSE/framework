#include "Listeners.h"

#include "Communication.h"
#include "Core.h"
#include "PrismaUI_API.h"

namespace PrismaUI::Listeners {
    using namespace Core;
    using namespace Communication;

    // MyLoadListener
    MyLoadListener::MyLoadListener(Core::PrismaViewId id) : viewId_(std::move(id)) {}

    MyLoadListener::~MyLoadListener() = default;

    void MyLoadListener::OnBeginLoading(View* /*caller*/, uint64_t /*frame_id*/, bool /*is_main_frame*/,
                                        const String& url) {
        logger::info("View [{}]: LoadListener: Begin loading URL: {}", viewId_, url.utf8().data());
    }

    void MyLoadListener::OnFinishLoading(View* /*caller*/, uint64_t /*frame_id*/, bool /*is_main_frame*/,
                                         const String& url) {
        logger::info("View [{}]: LoadListener: Finished loading URL: {}", viewId_, url.utf8().data());
        ultralightThread.submit([id = viewId_, urlStr = std::string(url.utf8().data())] {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(id);
            if (it != views.end()) {
                it->second->isLoadingFinished = true;
                it->second->lastLoadedUrl = urlStr;
                it->second->recoveryAttempts = 0;
                Communication::BindJSCallbacks(id);
            }
        });
    }

    void MyLoadListener::OnFailLoading(View* /*caller*/, uint64_t /*frame_id*/, bool /*is_main_frame*/,
                                       const String& url, const String& description, const String& /*error_domain*/,
                                       int /*error_code*/) {
        logger::error("View [{}]: LoadListener: Failed loading URL: {}. Error: {}", viewId_, url.utf8().data(),
                      description.utf8().data());
        ultralightThread.submit([id = viewId_] {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(id);
            if (it != views.end()) {
                it->second->isLoadingFinished = false;
            }
        });
    }

    void MyLoadListener::OnWindowObjectReady(View* /*caller*/, uint64_t /*frame_id*/, bool is_main_frame,
                                             const String& /*url*/) {
        if (is_main_frame) {
            logger::info("View [{}]: LoadListener: Window object ready.", viewId_);
        }
    }

    void MyLoadListener::OnDOMReady(View* /*caller*/, uint64_t /*frame_id*/, bool is_main_frame,
                                    const String& /*url*/) {
        if (is_main_frame) {
            logger::info("View [{}]: LoadListener: DOM ready.", viewId_);

            ultralightThread.submit([id = viewId_] {
                std::shared_lock lock(viewsMutex);
                auto it = views.find(id);
                if (it != views.end() && it->second->domReadyCallback) {
                    it->second->domReadyCallback(id);
                }
            });
        }
    }

    // MyViewListener
    MyViewListener::MyViewListener(Core::PrismaViewId id) : viewId_(std::move(id)) {}

    MyViewListener::~MyViewListener() = default;

    void MyViewListener::OnAddConsoleMessage(View* /*caller*/, const ConsoleMessage& message) {
        // logger::info("View [{}]: JSConsole: {}", viewId_, message.message().utf8().data());
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it != views.end() && it->second && it->second->consoleMessageCallback) {
            PRISMA_UI_API::ConsoleMessageLevel level = PRISMA_UI_API::ConsoleMessageLevel::Log;
            switch (message.level()) {
                case kMessageLevel_Warning: level = PRISMA_UI_API::ConsoleMessageLevel::Warning; break;
                case kMessageLevel_Error:   level = PRISMA_UI_API::ConsoleMessageLevel::Error; break;
                case kMessageLevel_Debug:   level = PRISMA_UI_API::ConsoleMessageLevel::Debug; break;
                case kMessageLevel_Info:    level = PRISMA_UI_API::ConsoleMessageLevel::Info; break;
                default: break;
            }
            auto msg = std::string(message.message().utf8().data());
            auto cb = it->second->consoleMessageCallback;
            auto id = viewId_;
            lock.unlock();
            cb(id, level, msg);
        }
    }

    RefPtr<View> MyViewListener::OnCreateInspectorView(View* /*caller*/, bool is_local, const String& inspectedURL) {
        logger::info(
            "View [{}]: ViewListener: OnCreateInspectorView called (is_local={}, "
            "URL={})",
            viewId_, is_local, inspectedURL.utf8().data());

        RefPtr<View> inspectorView = nullptr;

        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId_);
        if (it != views.end() && it->second) {
            auto viewData = it->second;

            if (!viewData->inspectorView && viewData->ultralightView && renderer) {
                uint32_t width = viewData->inspectorDisplayWidth > 0 ? viewData->inspectorDisplayWidth : 800;
                uint32_t height = viewData->inspectorDisplayHeight > 0 ? viewData->inspectorDisplayHeight : 600;

                ViewConfig config;
                config.is_accelerated = false;
                config.is_transparent = true;

                viewData->inspectorView = renderer->CreateView(width, height, config, nullptr);
                inspectorView = viewData->inspectorView;

                logger::info("View [{}]: Inspector view created with size {}x{}", viewId_, width, height);
            } else if (viewData->inspectorView) {
                inspectorView = viewData->inspectorView;
                logger::info("View [{}]: Returning existing inspector view", viewId_);
            }
        }

        return inspectorView;
    }

    // MyUltralightLogger
    MyUltralightLogger::~MyUltralightLogger() = default;

    void MyUltralightLogger::LogMessage(LogLevel /*log_level*/, const String& /*message*/) {
        // Implementation was empty, so keep it empty.
    }
}  // namespace PrismaUI::Listeners
