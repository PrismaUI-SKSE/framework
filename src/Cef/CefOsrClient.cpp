#include "PCH.h"

#ifdef GetNextSibling
#    undef GetNextSibling
#endif

#include "Cef/CefOsrClient.h"

#include <algorithm>

#include "include/wrapper/cef_helpers.h"

namespace
{
    constexpr int kCefWindowlessFrameRate = 120;

    const char* LogSeverityName(cef_log_severity_t level)
    {
        switch (level) {
            case LOGSEVERITY_VERBOSE:
                return "verbose";
            case LOGSEVERITY_INFO:
                return "info";
            case LOGSEVERITY_WARNING:
                return "warning";
            case LOGSEVERITY_ERROR:
                return "error";
            case LOGSEVERITY_FATAL:
                return "fatal";
            default:
                return "default";
        }
    }
}

namespace PrismaUI::Cef
{
    CefOsrClient::CefOsrClient(uint32_t width, uint32_t height) :
        width_(static_cast<int>(std::max<uint32_t>(1, width))),
        height_(static_cast<int>(std::max<uint32_t>(1, height)))
    {}

    bool CefOsrClient::HasBrowser() const
    {
        return hasBrowser_.load(std::memory_order_acquire);
    }

    void CefOsrClient::SetSize(uint32_t width, uint32_t height)
    {
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

    void CefOsrClient::SendExternalBeginFrame()
    {
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

    void CefOsrClient::CloseBrowser()
    {
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

    void CefOsrClient::ResetCloseSignal()
    {
        std::lock_guard lock(closeMutex_);
        closeComplete_ = false;
    }

    bool CefOsrClient::WaitForClose(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(closeMutex_);
        return closeCv_.wait_for(lock, timeout, [this]() { return closeComplete_; });
    }

    void CefOsrClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
    {
        CEF_REQUIRE_UI_THREAD();

        if (!browser) {
            logger::error("CEF OnAfterCreated received a null browser.");
            return;
        }

        if (browser->IsPopup()) {
            logger::warn("CEF popup browser [{}] was created during lifecycle smoke; closing it.", browser->GetIdentifier());
            browser->GetHost()->CloseBrowser(true);
            return;
        }

        browser_ = browser;
        hasBrowser_.store(true, std::memory_order_release);
        closing_.store(false, std::memory_order_release);
        browser_->GetHost()->SetWindowlessFrameRate(kCefWindowlessFrameRate);
        logger::info("CEF OSR browser [{}] created.", browser_->GetIdentifier());
    }

    bool CefOsrClient::DoClose(CefRefPtr<CefBrowser> browser)
    {
        CEF_REQUIRE_UI_THREAD();

        logger::info("CEF DoClose for browser [{}].", browser ? browser->GetIdentifier() : -1);
        closing_.store(true, std::memory_order_release);
        return false;
    }

    void CefOsrClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
    {
        CEF_REQUIRE_UI_THREAD();

        logger::info("CEF OnBeforeClose for browser [{}].", browser ? browser->GetIdentifier() : -1);

        if (!browser_ || !browser || browser_->IsSame(browser)) {
            browser_ = nullptr;
            hasBrowser_.store(false, std::memory_order_release);
            closing_.store(false, std::memory_order_release);
            SignalCloseComplete();
        }
    }

    void CefOsrClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect)
    {
        const int width = width_.load(std::memory_order_acquire);
        const int height = height_.load(std::memory_order_acquire);
        rect = CefRect(0, 0, std::max(1, width), std::max(1, height));
    }

    bool CefOsrClient::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screenInfo)
    {
        const int width = width_.load(std::memory_order_acquire);
        const int height = height_.load(std::memory_order_acquire);
        screenInfo.device_scale_factor = 1.0f;
        screenInfo.rect = CefRect(0, 0, std::max(1, width), std::max(1, height));
        screenInfo.available_rect = screenInfo.rect;
        return true;
    }

    void CefOsrClient::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirtyRects,
                               const void*, int width, int height)
    {
        if (type != PET_VIEW || width <= 0 || height <= 0) {
            return;
        }

        const uint64_t count = paintCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 300 == 0) {
            logger::debug("CEF CPU paint callback #{}: {}x{}, dirty rects {}", count, width, height, dirtyRects.size());
        }
    }

    void CefOsrClient::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirtyRects,
                                          const CefAcceleratedPaintInfo& info)
    {
        if (type != PET_VIEW) {
            return;
        }

        const uint64_t count = acceleratedPaintCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 300 == 0) {
            logger::debug("CEF accelerated paint callback #{}: shared handle {}, dirty rects {}", count,
                          info.shared_texture_handle ? "present" : "missing", dirtyRects.size());
        }
    }

    void CefOsrClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack,
                                            bool canGoForward)
    {
        logger::debug("CEF loading state browser [{}]: loading={}, canGoBack={}, canGoForward={}",
                      browser ? browser->GetIdentifier() : -1, isLoading, canGoBack, canGoForward);
    }

    void CefOsrClient::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                   TransitionType transitionType)
    {
        if (!frame || !frame->IsMain()) {
            return;
        }

        logger::info("CEF shell load started for browser [{}], transition type {}.",
                     browser ? browser->GetIdentifier() : -1, static_cast<int>(transitionType));
    }

    void CefOsrClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
    {
        if (!frame || !frame->IsMain()) {
            return;
        }

        logger::info("CEF shell load finished for browser [{}] with HTTP status {}.",
                     browser ? browser->GetIdentifier() : -1, httpStatusCode);
    }

    void CefOsrClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                                   const CefString& errorText, const CefString& failedUrl)
    {
        if (!frame || !frame->IsMain()) {
            return;
        }

        logger::error("CEF shell load failed for browser [{}]: code={}, error='{}', url='{}'",
                      browser ? browser->GetIdentifier() : -1, static_cast<int>(errorCode), errorText.ToString(),
                      failedUrl.ToString());
    }

    bool CefOsrClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                                        const CefString& message, const CefString& source, int line)
    {
        logger::info("CEF console [{}] browser [{}] {}:{} {}", LogSeverityName(level),
                     browser ? browser->GetIdentifier() : -1, source.ToString(), line, message.ToString());
        return false;
    }

    void CefOsrClient::SignalCloseComplete()
    {
        {
            std::lock_guard lock(closeMutex_);
            closeComplete_ = true;
        }
        closeCv_.notify_all();
    }
}
