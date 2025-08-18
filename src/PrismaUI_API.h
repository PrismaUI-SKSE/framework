/*
* For modders: Copy this file into your own project if you wish to use this API.
*/
#pragma once

#include <functional>
#include <queue>
#include <stdint.h>
#include <iostream>

typedef uint64_t PrismaView;

namespace PRISMA_UI_API
{
	constexpr const auto PrismaUIPluginName = "PrismaUI";

	using PluginHandle = SKSE::PluginHandle;
	using ActorHandle = RE::ActorHandle;

	enum class InterfaceVersion : uint8_t
	{
		V1
	};

	typedef void (*OnDomReadyCallback)(PrismaView view);
	typedef void (*JSCallback)(const char* result);
	typedef void (*JSListenerCallback)(const char* argument);

	// PrismaUI modder interface v1
	class IVPrismaUI1
	{
	public:
		// Create view.
		virtual PrismaView CreateView(const char* htmlPath, OnDomReadyCallback onDomReadyCallback = nullptr) noexcept = 0;

		// Send JS code to UI.
		virtual void Invoke(PrismaView view, const char* script, JSCallback callback = nullptr) noexcept = 0;

		// Call JS function through JS Interop API (best performance).
		virtual void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept = 0;

		// Register JS listener.
		virtual void RegisterJSListener(PrismaView view, const char* functionName, JSListenerCallback callback) noexcept = 0;

		// Returns true if view has focus.
		virtual bool HasFocus(PrismaView view) noexcept = 0;

		// Set focus on view.
		virtual bool Focus(PrismaView view, bool pauseGame = false) noexcept = 0;

		// Remove focus from view.
		virtual void Unfocus(PrismaView view) noexcept = 0;

		// Get scroll size in pixels.
		virtual int GetScrollingPixelSize(PrismaView view) noexcept = 0;

		// Set scroll size in pixels.
		virtual void SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept = 0;

		// Returns true if view exists.
		virtual bool IsValid(PrismaView view) noexcept = 0;

		// Completely destroy view.
		virtual void Destroy(PrismaView view) noexcept = 0;
	};

	typedef void* (*_RequestPluginAPI)(const InterfaceVersion interfaceVersion);

	/// <summary>
	/// Request the PrismaUI API interface.
	/// Recommended: Send your request during or after SKSEMessagingInterface::kMessage_PostLoad to make sure the dll has already been loaded
	/// </summary>
	/// <param name="a_interfaceVersion">The interface version to request</param>
	/// <returns>The pointer to the API singleton, or nullptr if request failed</returns>
	[[nodiscard]] inline void* RequestPluginAPI(const InterfaceVersion a_interfaceVersion = InterfaceVersion::V1)
	{
		auto pluginHandle = GetModuleHandle("PrismaUI.dll");
		_RequestPluginAPI requestAPIFunction = (_RequestPluginAPI)GetProcAddress(pluginHandle, "RequestPluginAPI");

		if (requestAPIFunction) {
			return requestAPIFunction(a_interfaceVersion);
		}

		return nullptr;
	}
}
