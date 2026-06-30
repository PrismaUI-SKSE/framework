#pragma once

class SingleThreadExecutor;

namespace PrismaUI::GamepadInputHandler {
    // Registers the gamepad input sink with BSInputDeviceManager and stores the Ultralight thread
    // executor used to fire events on the Renderer. Call once from InputHandler::Initialize.
    void Initialize(SingleThreadExecutor* ultralightThreadExecutor);

    // Drains queued gamepad events and fires them on the Renderer.
    // Call from InputHandler::ProcessEvents.
    void ProcessEvents();

    // Reset all "held" buttons so the next press registers as a fresh 0 -> 1 change.
    // Call on initialization and focus changes.
    // On focus loss it also pushes a neutral state to the Renderer so no button or axis stays latched
    // into the next focused view. (The input sink is gated on focus, so a release that lands while
    // unfocused would otherwise leave navigator.getGamepads() latched.) 
    void ResetButtonValues();

    // Removes the sink and clears queued state.
    void Shutdown();
}
