#pragma once

#include "PrismaUI_API.h"
#include "PrismaUI/ViewManager.h"

class PluginAPI {
    using LatestInterface = PRISMA_UI_API::IVPrismaUI3;

public:
    class PrismaUIInterface : public LatestInterface {
    private:
        PrismaUIInterface() noexcept {
        };

        virtual ~PrismaUIInterface() noexcept {
        };

    public:
        static PrismaUIInterface* GetSingleton() noexcept {
            static PrismaUIInterface singleton;
            return std::addressof(singleton);
        }

        // IVPrismaUI1 (order needs to match PrismaUI_API.h exactly for correct vtable layout)

        PrismaView CreateView(const char* htmlPath,
                              PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback = nullptr) noexcept override;
        void Invoke(PrismaView view, const char* script,
                    PRISMA_UI_API::JSCallback callback = nullptr) noexcept override;
        void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept override;
        void RegisterJSListener(PrismaView view, const char* fnName,
                                PRISMA_UI_API::JSListenerCallback callback) noexcept override;
        bool HasFocus(PrismaView view) noexcept override;
        bool Focus(PrismaView view, bool pauseGame = false, bool disableFocusMenu = false) noexcept override;
        void Unfocus(PrismaView view) noexcept override;
        void Show(PrismaView view) noexcept override;
        void Hide(PrismaView view) noexcept override;
        bool IsHidden(PrismaView view) noexcept override;
        int GetScrollingPixelSize(PrismaView view) noexcept override;
        void SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept override;
        bool IsValid(PrismaView view) noexcept override;
        void Destroy(PrismaView view) noexcept override;
        void SetOrder(PrismaView view, int order) noexcept override;
        int GetOrder(PrismaView view) noexcept override;
        void CreateInspectorView(PrismaView view) noexcept override;
        void SetInspectorVisibility(PrismaView view, bool visible) noexcept override;
        bool IsInspectorVisible(PrismaView view) noexcept override;
        void SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY, unsigned int width,
                                unsigned int height) noexcept override;
        bool HasAnyActiveFocus() noexcept override;

        // IVPrismaUI2

        void RegisterConsoleCallback(PrismaView view, PRISMA_UI_API::ConsoleMessageCallback callback) noexcept override;

        // IVPrismaUI3

        PrismaView CreateViewV2(
            const char* htmlPath, PRISMA_UI_API::OnDomReadyCallbackWithState onDomReadyCallback, void* callbackState) noexcept override;
        void InvokeV2(PrismaView view, const char* script, PRISMA_UI_API::JSCallbackWithState callback,
            void* callbackState) noexcept override;
        void RegisterJSListenerV2(PrismaView view, const char* functionName,
            PRISMA_UI_API::JSListenerCallbackWithState callback, void* callbackState) noexcept override;
        void RegisterConsoleCallbackV2(PrismaView view, PRISMA_UI_API::ConsoleMessageCallbackWithState callback,
            void* callbackState) noexcept override;

    private:
        static PrismaView CreateViewInternal(
            const char* htmlPath, std::function<void(PrismaUI::Core::PrismaViewId)> onDomReadyCallback) noexcept;
        static void InvokeInternal(
            PrismaView view, const char* script, std::function<void(const char*)> callback) noexcept;
        static void RegisterJSListenerInternal(PrismaView view, const char* functionName,
            std::function<void(const char*)> callback) noexcept;

        unsigned long apiTID = 0;
    };
};