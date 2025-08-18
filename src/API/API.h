#pragma once

#include <PrismaUI_API.h>
#include <PrismaUI/PrismaUI.h>

class PluginAPI
{
	using InterfaceVersion1 = PRISMA_UI_API::IVPrismaUI1;

public:
	class PrismaUIInterface : public InterfaceVersion1
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

		// InterfaceVersion1

		virtual PrismaView CreateView(const char* htmlPath, PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback = nullptr) noexcept override;
		virtual void Invoke(PrismaView view, const char* script, PRISMA_UI_API::JSCallback callback = nullptr) noexcept override;
		virtual void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept override;
		virtual void RegisterJSListener(PrismaView view, const char* fnName, PRISMA_UI_API::JSListenerCallback callback) noexcept override;
		virtual bool HasFocus(PrismaView view) noexcept override;
		virtual bool Focus(PrismaView view, bool pauseGame = false) noexcept override;
		virtual void Unfocus(PrismaView view) noexcept override;
		virtual int GetScrollingPixelSize(PrismaView view) noexcept override;
		virtual void SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept override;
		virtual bool IsValid(PrismaView view) noexcept override;
		virtual void Destroy(PrismaView view) noexcept override;

	private:
		unsigned long apiTID = 0;
	};
};