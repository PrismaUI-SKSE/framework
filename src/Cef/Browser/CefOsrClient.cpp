#include "PCH.h"

#ifdef GetNextSibling
    #undef GetNextSibling
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>

#include "Cef/Browser/CefOsrClient.h"
#include "Cef/Browser/CefRuntime.h"
#include "Cef/Shared/RendererToBrowserMessages.h"
#include "include/cef_process_message.h"
#include "include/cef_values.h"
#include "include/wrapper/cef_helpers.h"

namespace {
    constexpr int kCefWindowlessFrameRate = 120;

}

namespace PrismaUI::Cef {
    CefOsrClient::CefOsrClient(uint32_t width, uint32_t height)
        : width_(static_cast<int>(std::max<uint32_t>(1, width))),
          height_(static_cast<int>(std::max<uint32_t>(1, height))) {}

    bool CefOsrClient::HasBrowser() const { return hasBrowser_.load(std::memory_order_acquire); }

    void CefOsrClient::SetSize(uint32_t width, uint32_t height) {
        CEF_REQUIRE_UI_THREAD();

        const int newWidth = static_cast<int>(std::max<uint32_t>(1, width));
        const int newHeight = static_cast<int>(std::max<uint32_t>(1, height));
        const int oldWidth = width_.exchange(newWidth, std::memory_order_acq_rel);
        const int oldHeight = height_.exchange(newHeight, std::memory_order_acq_rel);

        if (oldWidth == newWidth && oldHeight == newHeight) {
            return;
        }

        logger::info("CEF OSR resize: {}x{} -> {}x{}", oldWidth, oldHeight, newWidth, newHeight);

        if (browser_) {
            browser_->GetHost()->WasResized();
            logger::debug("CEF OSR browser WasResized posted for {}x{}", newWidth, newHeight);
        }
    }

    void CefOsrClient::SendExternalBeginFrame() {
        CEF_REQUIRE_UI_THREAD();

        if (!browser_) {
            return;
        }

        const uint64_t frame = beginFrameCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (frame == 1 || frame % 300 == 0) {
            logger::debug("CEF external begin-frame request #{}", frame);
        }

        browser_->GetHost()->SendExternalBeginFrame();
    }

    void CefOsrClient::InvalidateView() {
        CEF_REQUIRE_UI_THREAD();

        if (!browser_) {
            return;
        }

        browser_->GetHost()->Invalidate(PET_VIEW);
        logger::debug("CEF OSR browser view invalidated for repaint.");
    }

    void CefOsrClient::CloseBrowser() {
        CEF_REQUIRE_UI_THREAD();

        if (!browser_) {
            logger::debug("CEF browser close requested, but no browser exists.");
            SignalCloseComplete();
            return;
        }

        logger::info("CEF browser close requested.");
        closing_.store(true, std::memory_order_release);
        browser_->GetHost()->CloseBrowser(false);
    }

    bool CefOsrClient::ConsumeCpuFrame(std::vector<std::byte>& pixels, uint32_t& width, uint32_t& height,
                                       uint32_t& stride) {
        if (!cpuFrameReady_.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }

        std::lock_guard lock(cpuFrameMutex_);
        if (cpuPixelBuffer_.empty() || cpuFrameWidth_ == 0 || cpuFrameHeight_ == 0 || cpuFrameStride_ == 0) {
            pixels.clear();
            width = 0;
            height = 0;
            stride = 0;
            return false;
        }

        pixels.swap(cpuPixelBuffer_);
        width = cpuFrameWidth_;
        height = cpuFrameHeight_;
        stride = cpuFrameStride_;
        cpuFrameWidth_ = 0;
        cpuFrameHeight_ = 0;
        cpuFrameStride_ = 0;
        return true;
    }

    void CefOsrClient::ResetCloseSignal() {
        std::lock_guard lock(closeMutex_);
        closeComplete_ = false;
    }

    bool CefOsrClient::WaitForClose(std::chrono::milliseconds timeout) {
        std::unique_lock lock(closeMutex_);
        return closeCv_.wait_for(lock, timeout, [this]() { return closeComplete_; });
    }

    CefRefPtr<CefBrowser> CefOsrClient::GetBrowserOnUiThread() const {
        CEF_REQUIRE_UI_THREAD();
        return browser_;
    }

    CefRefPtr<CefFrame> CefOsrClient::GetFrameByNameOnUiThread(const CefString& name) const {
        CEF_REQUIRE_UI_THREAD();
        return browser_ ? browser_->GetFrameByName(name) : nullptr;
    }

    void CefOsrClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
        CEF_REQUIRE_UI_THREAD();

        if (!browser) {
            logger::error("CEF OnAfterCreated received a null browser.");
            return;
        }

        if (browser->IsPopup()) {
            logger::warn("CEF popup browser [{}] was created during lifecycle smoke; closing it.",
                         browser->GetIdentifier());
            browser->GetHost()->CloseBrowser(true);
            return;
        }

        browser_ = browser;
        hasBrowser_.store(true, std::memory_order_release);
        closing_.store(false, std::memory_order_release);
        browser_->GetHost()->SetWindowlessFrameRate(kCefWindowlessFrameRate);
        logger::info("CEF OSR browser [{}] created.", browser_->GetIdentifier());
    }

    bool CefOsrClient::DoClose(CefRefPtr<CefBrowser> browser) {
        CEF_REQUIRE_UI_THREAD();

        logger::info("CEF DoClose for browser [{}].", browser ? browser->GetIdentifier() : -1);
        closing_.store(true, std::memory_order_release);
        return false;
    }

    void CefOsrClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
        CEF_REQUIRE_UI_THREAD();

        logger::info("CEF OnBeforeClose for browser [{}].", browser ? browser->GetIdentifier() : -1);

        if (!browser_ || !browser || browser_->IsSame(browser)) {
            browser_ = nullptr;
            hasBrowser_.store(false, std::memory_order_release);
            closing_.store(false, std::memory_order_release);
            SignalCloseComplete();
        }
    }

    void CefOsrClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
        const int width = width_.load(std::memory_order_acquire);
        const int height = height_.load(std::memory_order_acquire);
        rect = CefRect(0, 0, std::max(1, width), std::max(1, height));
    }

    bool CefOsrClient::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screenInfo) {
        const int width = width_.load(std::memory_order_acquire);
        const int height = height_.load(std::memory_order_acquire);
        screenInfo.device_scale_factor = 1.0f;
        screenInfo.rect = CefRect(0, 0, std::max(1, width), std::max(1, height));
        screenInfo.available_rect = screenInfo.rect;
        return true;
    }

    void CefOsrClient::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirtyRects,
                               const void* buffer, int width, int height) {
        if (type != PET_VIEW) {
            return;
        }

        if (!buffer || width <= 0 || height <= 0) {
            logger::warn("CEF CPU paint callback ignored invalid frame: buffer={}, size={}x{}", buffer ? "set" : "null",
                         width, height);
            return;
        }

        const auto frameWidth = static_cast<uint32_t>(width);
        const auto frameHeight = static_cast<uint32_t>(height);
        if (frameWidth > std::numeric_limits<uint32_t>::max() / 4U) {
            logger::error("CEF CPU paint callback ignored oversized frame width {}.", frameWidth);
            return;
        }

        const uint32_t stride = frameWidth * 4U;
        const size_t byteCount = static_cast<size_t>(frameHeight) * stride;
        if (byteCount == 0) {
            return;
        }

        try {
            std::lock_guard lock(cpuFrameMutex_);
            if (cpuPixelBuffer_.size() != byteCount) {
                cpuPixelBuffer_.resize(byteCount);
            }
            std::memcpy(cpuPixelBuffer_.data(), buffer, byteCount);
            cpuFrameWidth_ = frameWidth;
            cpuFrameHeight_ = frameHeight;
            cpuFrameStride_ = stride;
            cpuFrameReady_.store(true, std::memory_order_release);
        } catch (const std::exception& e) {
            logger::error("CEF CPU paint fallback failed to buffer {}x{} frame: {}", width, height, e.what());
            cpuFrameReady_.store(false, std::memory_order_release);
            return;
        }

        if (!cpuFallbackLogged_.exchange(true, std::memory_order_acq_rel)) {
            logger::warn("CEF CPU OnPaint fallback is active; GPU accelerated OSR is unavailable or delayed.");
        }

        const uint64_t count = paintCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 300 == 0) {
            logger::debug("CEF CPU paint callback #{}: {}x{}, dirty rects {}", count, width, height, dirtyRects.size());
        }
    }

    void CefOsrClient::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirtyRects,
                                          const CefAcceleratedPaintInfo& info) {
        if (type != PET_VIEW) {
            return;
        }

        const uint64_t count = acceleratedPaintCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 300 == 0) {
            logger::debug("CEF accelerated paint callback #{}: shared handle {}, dirty rects {}", count,
                          info.shared_texture_handle ? "present" : "missing", dirtyRects.size());
        }

        if (!info.shared_texture_handle) {
            logger::warn("CEF accelerated paint callback #{} did not include a shared texture handle.", count);
            return;
        }

        CefRuntime::GetSingleton().CopyAcceleratedFrameDuringCallback(info.shared_texture_handle);
    }

    bool CefOsrClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                                        const CefString& message, const CefString& /*source*/, int /*line*/) {
        const int browserId = browser ? browser->GetIdentifier() : -1;
        const std::string text = message.ToString();

        switch (level) {
            case LOGSEVERITY_ERROR:
            case LOGSEVERITY_FATAL:
                logger::error("CEF console browser [{}]: {}", browserId, text);
                break;
            case LOGSEVERITY_WARNING:
                logger::warn("CEF console browser [{}]: {}", browserId, text);
                break;
            case LOGSEVERITY_VERBOSE:
                logger::debug("CEF console browser [{}]: {}", browserId, text);
                break;
            case LOGSEVERITY_INFO:
            default:
                logger::info("CEF console browser [{}]: {}", browserId, text);
                break;
        }

        return true;
    }

    void CefOsrClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack,
                                            bool canGoForward) {
        logger::debug("CEF loading state browser [{}]: loading={}, canGoBack={}, canGoForward={}",
                      browser ? browser->GetIdentifier() : -1, isLoading, canGoBack, canGoForward);
    }

    void CefOsrClient::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                   TransitionType transitionType) {
        if (!frame) {
            return;
        }

        const std::string frameIdentifier = frame->GetIdentifier().ToString();
        const std::string url = frame->GetURL().ToString();
        if (frame->IsMain()) {
            logger::info("CEF shell load started for browser [{}], transition type {}, frame id '{}', url '{}'.",
                         browser ? browser->GetIdentifier() : -1, static_cast<int>(transitionType), frameIdentifier,
                         url);
            CefRuntime::GetSingleton().NotifyShellLoadStart(frameIdentifier, url);
            return;
        }

        const std::string frameName = frame->GetName().ToString();
        logger::info("CEF iframe load started: browser [{}], frame '{}', id '{}', url '{}'.",
                     browser ? browser->GetIdentifier() : -1, frameName, frameIdentifier, url);
        CefRuntime::GetSingleton().NotifyShellFrameLoadStart(frameName, frameIdentifier, url);
    }

    void CefOsrClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) {
        if (!frame) {
            return;
        }

        const std::string frameIdentifier = frame->GetIdentifier().ToString();
        const std::string url = frame->GetURL().ToString();
        if (frame->IsMain()) {
            logger::info("CEF shell load finished for browser [{}] with HTTP status {}, frame id '{}', url '{}'.",
                         browser ? browser->GetIdentifier() : -1, httpStatusCode, frameIdentifier, url);
            CefRuntime::GetSingleton().NotifyShellLoadEnd(httpStatusCode, frameIdentifier, url);
            return;
        }

        const std::string frameName = frame->GetName().ToString();
        logger::info("CEF iframe load finished: browser [{}], frame '{}', id '{}', status {}, url '{}'.",
                     browser ? browser->GetIdentifier() : -1, frameName, frameIdentifier, httpStatusCode, url);
        CefRuntime::GetSingleton().NotifyShellFrameLoadEnd(frameName, frameIdentifier, url, httpStatusCode);
    }

    void CefOsrClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                                   const CefString& errorText, const CefString& failedUrl) {
        if (!frame) {
            return;
        }

        const std::string frameIdentifier = frame->GetIdentifier().ToString();
        const std::string url = frame->GetURL().ToString();
        const std::string error = errorText.ToString();
        const std::string failed = failedUrl.ToString();
        if (frame->IsMain()) {
            logger::error(
                "CEF shell load failed for browser [{}]: code={}, error='{}', failedUrl='{}', frame id '{}', url='{}'",
                browser ? browser->GetIdentifier() : -1, static_cast<int>(errorCode), error, failed, frameIdentifier,
                url);
            CefRuntime::GetSingleton().NotifyShellLoadError(static_cast<int>(errorCode), error, failed, frameIdentifier,
                                                            url);
            return;
        }

        const std::string frameName = frame->GetName().ToString();
        logger::error(
            "CEF iframe load failed: browser [{}], frame '{}', id '{}', code={}, error='{}', failedUrl='{}', url='{}'",
            browser ? browser->GetIdentifier() : -1, frameName, frameIdentifier, static_cast<int>(errorCode), error,
            failed, url);
        CefRuntime::GetSingleton().NotifyShellFrameLoadError(frameName, frameIdentifier, url,
                                                             static_cast<int>(errorCode), error, failed);
    }

    bool CefOsrClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> frame,
                                                CefProcessId source_process, CefRefPtr<CefProcessMessage> message) {
        if (source_process != PID_RENDERER || !frame || !message) {
            return false;
        }

        auto deserializeResult =
            Messaging::DeserializeProcessMessageVariant<Messages::RendererToBrowserMessage>(message);
        return Match(
            deserializeResult,
            [&](const Messages::RendererToBrowserMessage& result) {
                return CefRuntime::GetSingleton().OnRendererMessage(frame->GetName(), result);
            },
            [](const Messaging::DeserializeMessageError e) {
                LOG(WARNING) << "CEF OnProcessMessageReceived failed to deserialize message";
                return false;
            });
    }

    void CefOsrClient::SignalCloseComplete() {
        {
            std::lock_guard lock(closeMutex_);
            closeComplete_ = true;
        }
        closeCv_.notify_all();
    }
}
