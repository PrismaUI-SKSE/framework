#include "ViewManager.h"

#include "Cef/CefRuntime.h"
#include "Core.h"
#include "InputHandler.h"
#include "ViewOperationQueue.h"

namespace PrismaUI::ViewManager {
    using namespace Core;

    namespace {
        // Apply the native side-effects of focusing a Prisma view (control-map disable,
        // FocusMenu open, optional pause, input capture). Caller already holds the target
        // viewData and has cleared focus on any other focused views.
        void ApplyFocusSideEffects(const Core::PrismaViewId& viewId, const std::shared_ptr<PrismaView>& viewData,
                                   bool pauseGame, bool disableFocusMenu) {
            PrismaUI::InputHandler::EnableInputCapture(viewId);

            if (!disableFocusMenu) {
                FocusMenu::Open();
            }

            if (auto* controlMap = RE::ControlMap::GetSingleton()) {
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kWheelZoom, false, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, false, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kJumping, false, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kMovement, false, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kActivate, false, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kPOVSwitch, false, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kVATS, false, false);
            }

            if (pauseGame) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->numPausesGame++;
                    viewData->isPaused.store(true);
                    logger::info("Focus: View [{}] paused the game.", viewId);
                }
            }
        }

        // Apply the native side-effects of unfocusing: blur the iframe via CEF, restore
        // controls, optionally close FocusMenu, drop the pause counter. closeFocusMenu==false
        // is used when transferring focus between Prisma views so the focus surface stays open.
        void ApplyUnfocusSideEffects(const Core::PrismaViewId& viewId, const std::shared_ptr<PrismaView>& viewData,
                                     bool closeFocusMenu) {
            if (viewData->isPaused.load()) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    if (ui->numPausesGame > 0) {
                        ui->numPausesGame--;
                    }
                }
                viewData->isPaused.store(false);
                logger::info("Unfocus: View [{}] released the pause counter.", viewId);
            }

            PrismaUI::InputHandler::DisableInputCapture(viewId);
            if (closeFocusMenu) {
                PrismaUI::InputHandler::ClearImeState(viewId);
            }

            Cef::CefRuntime::GetSingleton().BlurShellView(viewId);
            viewData->isFocused.store(false);

            if (closeFocusMenu) {
                FocusMenu::Close();
            }

            if (auto* controlMap = RE::ControlMap::GetSingleton()) {
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kWheelZoom, true, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, true, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kJumping, true, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kMovement, true, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kActivate, true, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kPOVSwitch, true, false);
                controlMap->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kVATS, true, false);
            }
        }

        std::shared_ptr<PrismaView> LookupView(const Core::PrismaViewId& viewId) {
            std::shared_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it == views.end()) return nullptr;
            return it->second;
        }
    }  // namespace

    Core::PrismaViewId Create(const std::string& htmlPath,
                              std::move_only_function<void(Core::PrismaViewId)> onDomReadyCallback) {
        bool expected_init = false;
        if (coreInitialized.compare_exchange_strong(expected_init, true)) {
            Core::InitializeCoreSystem();
            if (!renderer) {
                coreInitialized = false;
                logger::critical("Core initialization failed: Renderer not created.");
                throw std::runtime_error("PrismaUI Core Renderer initialization failed.");
            }
        } else if (!renderer) {
            logger::critical("Cannot create HTML view: Core Renderer is null despite initialization flag.");
            throw std::runtime_error("PrismaUI Core Renderer is unexpectedly null.");
        }

        const Core::PrismaViewId newViewId = generator.generate();

        // Mirror CefRuntime's URL resolution shape for logging consistency; the runtime
        // re-resolves the same way when CreateShellView is dispatched.
        std::string resolvedUrl;
        if (htmlPath.rfind("http://", 0) == 0 || htmlPath.rfind("https://", 0) == 0) {
            resolvedUrl = htmlPath;
        } else {
            resolvedUrl = "file:///views/" + htmlPath;
        }

        auto viewData = std::make_shared<Core::PrismaView>();
        viewData->id = newViewId;
        viewData->iframeName = "prisma-view-" + std::to_string(newViewId);
        viewData->resolvedUrl = resolvedUrl;
        viewData->originalUrl = resolvedUrl;  // retained for Step 11 recovery policy
        viewData->isHidden = false;
        viewData->domReadyCallback = std::move(onDomReadyCallback);

        {
            std::unique_lock lock(viewsMutex);
            int maxOrder = -1;
            for (const auto& pair : views) {
                if (pair.second && pair.second->order > maxOrder) {
                    maxOrder = pair.second->order;
                }
            }
            viewData->order = maxOrder + 1;
            views[newViewId] = viewData;
        }

        logger::info("View [{}] create requested: iframe={}, url={}, order={}", newViewId, viewData->iframeName,
                     resolvedUrl, viewData->order);

        // Enqueue the actual CEF shell command. CefRuntime caches the request and replays it
        // once the shell page is ready, so we deliberately do not block on shell readiness.
        const std::string htmlPathCopy = htmlPath;
        const int order = viewData->order;
        ViewOperationQueue::EnqueueOperation(newViewId, [newViewId, htmlPathCopy, order]() {
            auto view = LookupView(newViewId);
            if (!view) return;
            bool expected = false;
            if (!view->iframeCreateRequested.compare_exchange_strong(expected, true)) {
                logger::debug("Create: View [{}] iframe already requested; skipping duplicate.", newViewId);
                return;
            }

            const bool ok =
                Cef::CefRuntime::GetSingleton().CreateShellView(newViewId, htmlPathCopy, order, /*hidden=*/false);
            if (!ok) {
                logger::error("Create: CefRuntime::CreateShellView returned false for View [{}] (iframe={}).",
                              newViewId, view->iframeName);
            } else {
                logger::info("Create: View [{}] iframe={} dispatched to CEF shell.", newViewId, view->iframeName);
            }
        });

        return newViewId;
    }

    void Show(const Core::PrismaViewId& viewId) {
        if (!IsValid(viewId)) {
            logger::warn("Show: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            auto viewData = LookupView(viewId);
            if (!viewData) return;

            if (!viewData->isHidden.load()) {
                logger::debug("Show: View [{}] is already visible.", viewId);
                return;
            }
            viewData->isHidden.store(false);
            Cef::CefRuntime::GetSingleton().SetShellViewHidden(viewId, false);
            logger::info("Show: View [{}] (iframe={}) marked visible.", viewId, viewData->iframeName);
        });
    }

    void Hide(const Core::PrismaViewId& viewId) {
        if (!IsValid(viewId)) {
            logger::warn("Hide: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            auto viewData = LookupView(viewId);
            if (!viewData) return;

            if (viewData->isHidden.load()) {
                logger::debug("Hide: View [{}] is already hidden.", viewId);
                return;
            }

            if (viewData->isFocused.load()) {
                ApplyUnfocusSideEffects(viewId, viewData, /*closeFocusMenu=*/true);
                logger::info("Hide: View [{}] was focused; unfocused before hiding.", viewId);
            }

            viewData->isHidden.store(true);
            Cef::CefRuntime::GetSingleton().SetShellViewHidden(viewId, true);
            logger::info("Hide: View [{}] (iframe={}) marked hidden.", viewId, viewData->iframeName);
        });
    }

    bool IsHidden(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end()) {
            return it->second->isHidden.load();
        }
        logger::warn("IsHidden: View ID [{}] not found.", viewId);
        return true;
    }

    bool IsValid(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        return views.find(viewId) != views.end();
    }

    bool Focus(const Core::PrismaViewId& viewId, bool pauseGame, bool disableFocusMenu) {
        if (!IsValid(viewId)) {
            logger::warn("Focus: View ID [{}] not found.", viewId);
            return false;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId, pauseGame, disableFocusMenu]() {
            auto viewData = LookupView(viewId);
            if (!viewData) {
                logger::warn("Focus: View [{}] disappeared before focus could be applied.", viewId);
                return;
            }

            if (viewData->isHidden.load()) {
                logger::warn("Focus: View [{}] is hidden; cannot focus.", viewId);
                return;
            }

            if (viewData->isFocused.load()) {
                logger::debug("Focus: View [{}] already focused.", viewId);
                return;
            }

            // Queue unfocus on every other currently-focused view. We pass closeFocusMenu=false
            // so the focus surface stays open during transfer.
            std::vector<Core::PrismaViewId> viewsToUnfocus;
            {
                std::shared_lock lock(viewsMutex);
                for (const auto& pair : views) {
                    if (pair.first != viewId && pair.second && pair.second->isFocused.load()) {
                        viewsToUnfocus.push_back(pair.first);
                    }
                }
            }

            for (const auto& idToUnfocus : viewsToUnfocus) {
                ViewOperationQueue::EnqueueOperation(idToUnfocus, [idToUnfocus]() {
                    auto vd = LookupView(idToUnfocus);
                    if (!vd) return;
                    if (!vd->isFocused.load()) return;
                    ApplyUnfocusSideEffects(idToUnfocus, vd, /*closeFocusMenu=*/false);
                    logger::info("Focus: View [{}] unfocused (focus switching).", idToUnfocus);
                });
            }

            viewData->isFocused.store(true);
            Cef::CefRuntime::GetSingleton().FocusShellView(viewId);
            ApplyFocusSideEffects(viewId, viewData, pauseGame, disableFocusMenu);

            logger::info("Focus: View [{}] (iframe={}) focused: pauseGame={}, focusMenu={}", viewId,
                         viewData->iframeName, pauseGame, !disableFocusMenu);
        });

        return true;
    }

    void Unfocus(const Core::PrismaViewId& viewId) {
        if (!IsValid(viewId)) {
            logger::warn("Unfocus: View ID [{}] not found.", viewId);
            return;
        }

        ViewOperationQueue::EnqueueOperation(viewId, [viewId]() {
            auto viewData = LookupView(viewId);
            if (!viewData) {
                logger::warn("Unfocus: View [{}] disappeared before unfocus could be applied.", viewId);
                PrismaUI::InputHandler::DisableInputCapture(0);
                FocusMenu::Close();
                return;
            }

            if (!viewData->isFocused.load()) {
                logger::debug("Unfocus: View [{}] was not focused.", viewId);
                return;
            }

            ApplyUnfocusSideEffects(viewId, viewData, /*closeFocusMenu=*/true);
            logger::info("Unfocus: View [{}] (iframe={}) unfocused.", viewId, viewData->iframeName);
        });
    }

    bool HasFocus(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it == views.end()) {
            logger::warn("HasFocus: View ID [{}] not found.", viewId);
            return false;
        }
        return it->second->isFocused.load();
    }

    bool ViewHasInputFocus(const Core::PrismaViewId& viewId) {
        // Step 6 simplification: native focus state is the source of truth.
        // Step 7 may refine using DOM activeElement signalling from CEF.
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it == views.end()) return false;
        return it->second->isFocused.load();
    }

    void SetScrollingPixelSize(const Core::PrismaViewId& viewId, int pixelSize) {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it == views.end()) {
            logger::warn("SetScrollingPixelSize: View ID [{}] not found.", viewId);
            return;
        }
        if (pixelSize <= 0) {
            logger::warn("SetScrollingPixelSize: Invalid pixel size {} for view [{}]. Must be > 0. Using default.",
                         pixelSize, viewId);
            it->second->scrollingPixelSize = 16;
        } else {
            it->second->scrollingPixelSize = pixelSize;
            logger::debug("SetScrollingPixelSize: Set {} pixels per scroll line for view [{}]", pixelSize, viewId);
        }
    }

    int GetScrollingPixelSize(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end()) {
            return it->second->scrollingPixelSize;
        }
        logger::warn("GetScrollingPixelSize: View ID [{}] not found, returning default.", viewId);
        return 28;
    }

    void Destroy(const Core::PrismaViewId& viewId) {
        logger::info("Destroy: Beginning destruction of View [{}]", viewId);

        if (!IsValid(viewId)) {
            logger::warn("Destroy: View ID [{}] not found.", viewId);
            return;
        }

        // Drop any pending operations so they cannot race against destruction.
        ViewOperationQueue::ClearOperations(viewId);

        std::shared_ptr<PrismaView> viewDataToDestroy;
        {
            std::unique_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it == views.end()) {
                logger::warn("Destroy: View ID [{}] not found after revalidation.", viewId);
                return;
            }
            viewDataToDestroy = std::move(it->second);
            views.erase(it);
        }

        viewDataToDestroy->destroyRequested.store(true, std::memory_order_release);

        // If the view was focused, run the unfocus side-effects inline. The operation queue
        // is already drained, and the view has been removed from the map so no peer can race.
        if (viewDataToDestroy->isFocused.load()) {
            ApplyUnfocusSideEffects(viewId, viewDataToDestroy, /*closeFocusMenu=*/true);
            logger::info("Destroy: View [{}] was focused; applied unfocus side-effects inline.", viewId);
        }

        viewDataToDestroy->isHidden.store(true);

        // Remove any JS callbacks registered for this view.
        {
            std::lock_guard<std::mutex> lock(jsCallbacksMutex);
            std::size_t removed = 0;
            for (auto it = jsCallbacks.begin(); it != jsCallbacks.end();) {
                if (it->first.first == viewId) {
                    it = jsCallbacks.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            if (removed > 0) {
                logger::debug("Destroy: Removed {} JavaScript callback(s) for View [{}]", removed, viewId);
            }
        }

        // Drain any in-flight Invoke callbacks so the caller gets one final empty
        // string instead of being left holding a never-fired callback.
        Cef::CefRuntime::GetSingleton().CancelInvokesForView(viewId);

        Cef::CefRuntime::GetSingleton().DestroyShellView(viewId);

        logger::info("Destroy: View [{}] (iframe={}) destroyed.", viewId, viewDataToDestroy->iframeName);
    }

    void SetOrder(const Core::PrismaViewId& viewId, int order) {
        std::shared_ptr<PrismaView> viewData;
        {
            std::unique_lock lock(viewsMutex);
            auto it = views.find(viewId);
            if (it == views.end()) {
                logger::warn("SetOrder: View ID [{}] not found.", viewId);
                return;
            }
            it->second->order = order;
            viewData = it->second;
        }

        Cef::CefRuntime::GetSingleton().SetShellViewOrder(viewId, order);
        logger::info("SetOrder: View [{}] (iframe={}) order set to {}.", viewId, viewData->iframeName, order);
    }

    int GetOrder(const Core::PrismaViewId& viewId) {
        std::shared_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end()) {
            return it->second->order;
        }
        logger::warn("GetOrder: View ID [{}] not found, returning -1.", viewId);
        return -1;
    }

    // ========== Inspector API Wrappers (Step 6 stubs) ==========
    // Inspector V3 API stays in the vtable but every call is a deprecation log + no-op.
    // Step 9 introduces a new API version with proper DevTools verbs.

    namespace {
        template <typename Tag>
        void LogDeprecatedOnce(const char* method, const Core::PrismaViewId& viewId) {
            static std::atomic<bool> logged{false};
            bool expected = false;
            if (logged.compare_exchange_strong(expected, true)) {
                logger::warn(
                    "{}: Inspector API is deprecated and superseded by the DevTools API in Step 9. "
                    "Call against View [{}] is a no-op.",
                    method, viewId);
            }
        }
    }

    struct InspectorCreateTag {};
    struct InspectorVisibilityTag {};
    struct InspectorIsVisibleTag {};
    struct InspectorBoundsTag {};

    void CreateInspectorView(const Core::PrismaViewId& viewId) {
        LogDeprecatedOnce<InspectorCreateTag>("CreateInspectorView", viewId);
    }

    void SetInspectorVisibility(const Core::PrismaViewId& viewId, bool /*visible*/) {
        LogDeprecatedOnce<InspectorVisibilityTag>("SetInspectorVisibility", viewId);
    }

    bool IsInspectorVisible(const Core::PrismaViewId& viewId) {
        LogDeprecatedOnce<InspectorIsVisibleTag>("IsInspectorVisible", viewId);
        return false;
    }

    void SetInspectorBounds(const Core::PrismaViewId& viewId, float /*topLeftX*/, float /*topLeftY*/,
                            uint32_t /*width*/, uint32_t /*height*/) {
        LogDeprecatedOnce<InspectorBoundsTag>("SetInspectorBounds", viewId);
    }

    bool HasAnyActiveFocus() {
        std::shared_lock lock(viewsMutex);
        for (const auto& pair : views) {
            if (pair.second && pair.second->isFocused.load()) {
                return true;
            }
        }
        return false;
    }

    void RegisterConsoleCallback(
        const Core::PrismaViewId& viewId,
        std::move_only_function<void(PrismaViewId, PRISMA_UI_API::ConsoleMessageLevel, const std::string&)> callback) {
        std::unique_lock lock(viewsMutex);
        auto it = views.find(viewId);
        if (it != views.end() && it->second) {
            it->second->consoleMessageCallback = std::move(callback);
        } else {
            logger::warn("RegisterConsoleCallback: View ID [{}] not found.", viewId);
        }
    }
}
