#pragma once

#include "include/cef_render_process_handler.h"

namespace PrismaUI::Cef {
    // CefRenderProcessHandler implementation that runs inside CEF renderer
    // subprocesses. Hosts the V8 bridge that powers Invoke / InteropCall /
    // RegisterJSListener / DOM-ready / console-message routing for any iframe
    // whose name matches "prisma-view-<id>".
    //
    // Instances of this class are constructed in the plugin DLL too (since
    // PrismaCefApp::GetRenderProcessHandler() returns one unconditionally) but
    // CEF only ever invokes the handler methods on the renderer process main
    // thread (TID_RENDERER), so the browser-side instance is dormant.
    class PrismaCefRenderApp final : public CefRenderProcessHandler {
    public:
        PrismaCefRenderApp();
        ~PrismaCefRenderApp() override;

        void OnContextCreated(CefRefPtr<CefBrowser> browser,
                              CefRefPtr<CefFrame> frame,
                              CefRefPtr<CefV8Context> context) override;
        void OnContextReleased(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               CefRefPtr<CefV8Context> context) override;
        bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      CefProcessId source_process,
                                      CefRefPtr<CefProcessMessage> message) override;

    private:
        struct Impl;
        Impl* impl_;

        IMPLEMENT_REFCOUNTING(PrismaCefRenderApp);
    };
}
