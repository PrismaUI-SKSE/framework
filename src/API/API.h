#pragma once

#include "PrismaUI_API.h"

class PluginAPI
{
	using LatestInterface = PRISMA_UI_API::IVPrismaUI2;

public:
	class PrismaUIInterface : public LatestInterface
	{
	private:
		PrismaUIInterface() noexcept {};
		virtual ~PrismaUIInterface() noexcept {};

	public:
		static PrismaUIInterface* GetSingleton() noexcept
		{
			static PrismaUIInterface singleton;
			return std::addressof(singleton);
		}

		// IVPrismaUI1 (order needs to match PrismaUI_API.h exactly for correct vtable layout)

		virtual PrismaView CreateView(const char* htmlPath, PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback = nullptr) noexcept override;
		virtual void Invoke(PrismaView view, const char* script, PRISMA_UI_API::JSCallback callback = nullptr) noexcept override;
		virtual void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept override;
		virtual void RegisterJSListener(PrismaView view, const char* fnName, PRISMA_UI_API::JSListenerCallback callback) noexcept override;
		virtual bool HasFocus(PrismaView view) noexcept override;
		virtual bool Focus(PrismaView view, bool pauseGame = false, bool disableFocusMenu = false) noexcept override;
		virtual void Unfocus(PrismaView view) noexcept override;
		virtual void Show(PrismaView view) noexcept override;
		virtual void Hide(PrismaView view) noexcept override;
		virtual bool IsHidden(PrismaView view) noexcept override;
		virtual int GetScrollingPixelSize(PrismaView view) noexcept override;
		virtual void SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept override;
		virtual bool IsValid(PrismaView view) noexcept override;
		virtual void Destroy(PrismaView view) noexcept override;
		virtual void SetOrder(PrismaView view, int order) noexcept override;
		virtual int GetOrder(PrismaView view) noexcept override;
		virtual void CreateInspectorView(PrismaView view) noexcept override;
		virtual void SetInspectorVisibility(PrismaView view, bool visible) noexcept override;
		virtual bool IsInspectorVisible(PrismaView view) noexcept override;
		virtual void SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY, unsigned int width, unsigned int height) noexcept override;
		virtual bool HasAnyActiveFocus() noexcept override;

		// IVPrismaUI2

		virtual void RegisterConsoleCallback(PrismaView view, PRISMA_UI_API::ConsoleMessageCallback callback) noexcept override;

	private:
		unsigned long apiTID = 0;
	};
};
