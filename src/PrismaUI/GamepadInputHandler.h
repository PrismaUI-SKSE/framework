#pragma once

class SingleThreadExecutor;

namespace PrismaUI::GamepadInputHandler {
    // Registers the gamepad input sink with BSInputDeviceManager and stores the Ultralight thread
    // executor used to fire events on the Renderer. Call once from InputHandler::Initialize.
    void Initialize(SingleThreadExecutor* ultralightThreadExecutor);

    // Drains queued gamepad events and fires them on the Renderer.
    // Call from InputHandler::ProcessEvents.
    void ProcessEvents();

    // Reset all "held" buttons so the next press registers as a fresh 0 -> 1 change. This function is called on
    // initialization and focus changes.
    void ResetButtonValues();

    // Removes the sink and clears queued state.
    void Shutdown();
}
