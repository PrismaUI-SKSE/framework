#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"

namespace PrismaUI::Cef
{
    class CefOsrClient final : public CefClient,
                               public CefRenderHandler,
                               public CefLifeSpanHandler,
                               public CefLoadHandler,
                               public CefDisplayHandler
    {
    public:
        CefOsrClient(uint32_t width, uint32_t height);

        CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
        CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
        CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

        bool HasBrowser() const;
        void SetSize(uint32_t width, uint32_t height);
        void SendExternalBeginFrame();
        void CloseBrowser();
        void ResetCloseSignal();
        bool WaitForClose(std::chrono::milliseconds timeout);

        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
        bool DoClose(CefRefPtr<CefBrowser> browser) override;
        void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

        void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
        bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screenInfo) override;
        void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects,
                     const void* buffer, int width, int height) override;
        void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects,
                                const CefAcceleratedPaintInfo& info) override;

        void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack,
                                  bool canGoForward) override;
        void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                         TransitionType transitionType) override;
        void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
        void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                         const CefString& errorText, const CefString& failedUrl) override;
        bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message,
                              const CefString& source, int line) override;

    private:
        void SignalCloseComplete();

        std::atomic<int> width_;
        std::atomic<int> height_;
        std::atomic<bool> hasBrowser_ = false;
        std::atomic<bool> closing_ = false;
        std::atomic<uint64_t> beginFrameCount_ = 0;
        std::atomic<uint64_t> paintCount_ = 0;
        std::atomic<uint64_t> acceleratedPaintCount_ = 0;

        CefRefPtr<CefBrowser> browser_;
        std::mutex closeMutex_;
        std::condition_variable closeCv_;
        bool closeComplete_ = false;

        IMPLEMENT_REFCOUNTING(CefOsrClient);
    };
}
