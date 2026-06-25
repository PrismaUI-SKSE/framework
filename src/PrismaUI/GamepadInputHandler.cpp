#include "GamepadInputHandler.h"

// Ultralight headers trip C4100 (unreferenced formal parameter); silence it for them only.
#pragma warning(push)
#pragma warning(disable : 4100)
#include <Ultralight/Ultralight.h>
#pragma warning(pop)

#include <atomic>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Communication.h"
#include "Core.h"
#include "InputHandler.h"
#include "PrismaVR.h"
#include "Utils/SingleThreadExecutor.h"

// Gamepad Support: Ultralight will not automatically detect a gamepad.
// This code uses BSInputDeviceManager to monitor button presses.
// It informs Ultralight via Renderer::FireGamepad* so navigator.getGamepads() can function normally.
// The controller is always at index 0 and mimicks the standard W3C controller:
// https://www.w3.org/TR/gamepad/#remapping
//
// Threading: input is captured and queued on the game's input thread; the queue is drained and
// fired on the Renderer from the Ultralight thread, because Renderer calls are single-threaded.
namespace PrismaUI::GamepadInputHandler {
    namespace {
        constexpr uint32_t GAMEPAD_INDEX = 0;          // The gamepad is always depicted at index 0.
        constexpr uint32_t GAMEPAD_AXIS_COUNT = 4;     // W3C standard gamepad with left and right X and Y axes
        constexpr uint32_t GAMEPAD_BUTTON_COUNT = 17;  // W3C standard gamepad with 17 buttons

        // A queued gamepad event: a button change or an axis change.
        using GamepadQueuedEvent = std::variant<ultralight::GamepadButtonEvent, ultralight::GamepadAxisEvent>;
        std::mutex g_gamepadQueueMutex;  // Guards g_gamepadQueue across the input and Ultralight threads
        std::vector<GamepadQueuedEvent>
            g_gamepadQueue;  // Input thread adds to this vector. Ultralight thread consumes.

        // Allows execution on the Ultralight thread. Calls Core::Renderer. Owned by InputHandler.
        SingleThreadExecutor* g_ultralightThreadExecutor = nullptr;
        // Ensure virtual gamepad is declared to the Renderer once upon the first input.
        std::atomic<bool> g_gamepadRegistered = false;
        float g_gamepadButtonValues[GAMEPAD_BUTTON_COUNT] = {};  // Last observed value per button.

        // Converts Skyrim ID Code to W3C index (-1 if not recognized, e.g., guide button)
        int SkyrimIDCodeToW3CIndex(uint32_t skyrimCode) {
            using Key = RE::BSWin32GamepadDevice::Key;
            return skyrimCode == Key::kA               ? 0
                   : skyrimCode == Key::kB             ? 1
                   : skyrimCode == Key::kX             ? 2
                   : skyrimCode == Key::kY             ? 3
                   : skyrimCode == Key::kLeftShoulder  ? 4
                   : skyrimCode == Key::kRightShoulder ? 5
                   : skyrimCode == Key::kLeftTrigger   ? 6  // analog
                   : skyrimCode == Key::kRightTrigger  ? 7  // analog
                   : skyrimCode == Key::kBack          ? 8
                   : skyrimCode == Key::kStart         ? 9
                   : skyrimCode == Key::kLeftThumb     ? 10
                   : skyrimCode == Key::kRightThumb    ? 11
                   : skyrimCode == Key::kUp            ? 12
                   : skyrimCode == Key::kDown          ? 13
                   : skyrimCode == Key::kLeft          ? 14
                   : skyrimCode == Key::kRight         ? 15
                                                       : -1;
        }

        // Resolves the menu role of the button via the control map and dispatches a named CustomEvent
        // (detail: { w3cButtonIndex: number, skyrimIdCode: number, action: string }) to the focused view.
        // eventName is "prismagamepadbuttondown" on a press or "prismagamepadbuttonup" on a release.
        void DispatchButtonEvent(const char* eventName, uint32_t w3cButtonIndex, uint32_t skyrimIDCode) {
            const Core::PrismaViewId viewId = InputHandler::GetFocusedViewId();
            if (viewId == 0) {
                return;
            }

            const char* action = "";
            if (auto* controlMap = RE::ControlMap::GetSingleton()) {
                using Context = RE::ControlMap::InputContextID;
                const uint32_t acceptIDCode =
                    controlMap->GetMappedKey("Accept", RE::INPUT_DEVICE::kGamepad, Context::kMenuMode);
                if (acceptIDCode == skyrimIDCode) {
                    action = "accept";
                } else {
                    const uint32_t cancelIDCode =
                        controlMap->GetMappedKey("Cancel", RE::INPUT_DEVICE::kGamepad, Context::kMenuMode);
                    if (cancelIDCode == skyrimIDCode) {
                        action = "cancel";
                    }
                }
            }

            const std::string script = std::string("window.dispatchEvent(new CustomEvent(\"") + eventName +
                                       "\", {detail: {w3cButtonIndex: " + std::to_string(w3cButtonIndex) +
                                       ", skyrimIdCode: " + std::to_string(skyrimIDCode) + ", action: \"" + action +
                                       "\"}}))";
            Communication::Invoke(viewId, script.c_str());
        }

        // --- Input thread: capture into the queue ---

        // Maps a gamepad button event to the standard layout and queues it, skipping unchanged repeats.
        void QueueButtonEvent(RE::ButtonEvent* buttonEvent) {
            const uint32_t buttonIDCode = buttonEvent->GetIDCode();
            const int w3cButtonIndexInt = SkyrimIDCodeToW3CIndex(buttonIDCode);  // Skyrim code -> W3C index.
            if (w3cButtonIndexInt < 0) {
                // Unrecognized input
                return;
            }

            const uint32_t w3cButtonIndex = static_cast<uint32_t>(w3cButtonIndexInt);

            // Fire buttondown on the press edge and buttonup on the release edge.
            if (buttonEvent->IsDown()) {
                DispatchButtonEvent("prismagamepadbuttondown", w3cButtonIndex, buttonIDCode);
            } else if (buttonEvent->IsUp()) {
                DispatchButtonEvent("prismagamepadbuttonup", w3cButtonIndex, buttonIDCode);
            }

            // A trigger ranges from 0 to 1 (float); a digital button returns 0 or 1.
            const float value = buttonEvent->Value();
            if (g_gamepadButtonValues[w3cButtonIndex] == value) {
                // Button value unchanged since last event. Don't send unnecessarily.
                return;
            }
            // Save button value for future calls to this function.
            g_gamepadButtonValues[w3cButtonIndex] = value;

            // Build event to be received by the Renderer
            ultralight::GamepadButtonEvent ev{};
            ev.index = GAMEPAD_INDEX;
            ev.button_index = w3cButtonIndex;
            ev.value = value;

            // Add to gamepad queue so it can be picked up later on the Ultralight thread.
            std::lock_guard lock(g_gamepadQueueMutex);
            g_gamepadQueue.emplace_back(ev);
        }

        // Maps a thumbstick event to its standard axis pair and queues both axes together.
        void QueueThumbstickEvent(RE::ThumbstickEvent* thumbstickEvent) {
            // Pick the standard axis pair for whichever stick moved.
            uint32_t xAxis;
            uint32_t yAxis;
            if (thumbstickEvent->IsLeft()) {
                xAxis = 0;
                yAxis = 1;
            } else if (thumbstickEvent->IsRight()) {
                xAxis = 2;
                yAxis = 3;
            } else {
                // If neither left nor right, ignore.
                return;
            }

            // Build x-axis event to be received by the Renderer.
            ultralight::GamepadAxisEvent xev{};
            xev.index = GAMEPAD_INDEX;
            xev.axis_index = xAxis;
            xev.value = thumbstickEvent->xValue;

            // Build y-axis event to be received by the Renderer.
            ultralight::GamepadAxisEvent yev{};
            yev.index = GAMEPAD_INDEX;
            yev.axis_index = yAxis;
            // Negate Y: Skyrim reports +up while the W3C standard mapping expects +down.
            yev.value = -thumbstickEvent->yValue;

            // Add to gamepad queue so it can be picked up later on the Ultralight thread.
            std::lock_guard lock(g_gamepadQueueMutex);
            g_gamepadQueue.emplace_back(xev);
            g_gamepadQueue.emplace_back(yev);
        }

        // Captures Skyrim gamepad input and queues it for ProcessEvents(). Gated on input capture, like mouse/keyboard.
        class GamepadEventListener : public RE::BSTEventSink<RE::InputEvent*> {
        public:
            static GamepadEventListener* GetSingleton() {
                // One instance for the process. Its address is used in AddEventSink and RemoveEventSink.
                static GamepadEventListener singleton;
                return &singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* a_event,
                [[maybe_unused]] RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override {
                // If event is valid, input capture is active, and not VR, queue events.
                if (a_event && *a_event && InputHandler::IsAnyInputCaptureActive() && !PrismaVR::IsVRActive()) {
                    for (auto event = *a_event; event; event = event->next) {  // Events arrive as a linked list.
                        const RE::INPUT_EVENT_TYPE eventType = event->GetEventType();
                        if (eventType == RE::INPUT_EVENT_TYPE::kButton) {
                            // For buttons, ensure the device is the gamepad.
                            auto buttonEvent = event->AsButtonEvent();
                            if (buttonEvent && buttonEvent->GetDevice() == RE::INPUT_DEVICE::kGamepad) {
                                QueueButtonEvent(buttonEvent);
                            }
                        } else if (eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
                            // For thumbsticks, the device is always a gamepad.
                            if (auto thumbstickEvent = event->AsThumbstickEvent()) {
                                QueueThumbstickEvent(thumbstickEvent);
                            }
                        }
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // --- Ultralight thread: drain onto the Renderer ---

        // Declares the virtual pad to the Renderer exactly once so navigator.getGamepads() reports it.
        void EnsureGamepadRegistered() {
            if (g_gamepadRegistered.exchange(true)) {
                // If already registered, return.
                return;
            }
            const ultralight::String name = "Skyrim Controller (Standard Mapping)";
            Core::renderer->SetGamepadDetails(GAMEPAD_INDEX, name, GAMEPAD_AXIS_COUNT, GAMEPAD_BUTTON_COUNT);
            ultralight::GamepadEvent connectEvent{};
            connectEvent.type =
                ultralight::GamepadEvent::kType_GamepadConnected;  // JavaScript "gamepadconnected" event
            connectEvent.index = GAMEPAD_INDEX;
            Core::renderer->FireGamepadEvent(connectEvent);
        }

        // Fires one queued event on the Renderer, picking the call that matches its variant type
        // This does not trigger any JavaScript event. It just changes gamepad state.
        void FireGamepadQueuedEvent(const GamepadQueuedEvent& event) {
            std::visit(
                [](const auto& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, ultralight::GamepadButtonEvent>) {
                        Core::renderer->FireGamepadButtonEvent(arg);
                    } else if constexpr (std::is_same_v<T, ultralight::GamepadAxisEvent>) {
                        Core::renderer->FireGamepadAxisEvent(arg);
                    }
                },
                event);
        }
    }

    void ResetButtonValues() {
        for (auto& v : g_gamepadButtonValues) {
            v = 0.0f;
        }
    }

    void Initialize(SingleThreadExecutor* ultralightThreadExecutor) {
        g_ultralightThreadExecutor = ultralightThreadExecutor;
        g_gamepadRegistered = false;  // Declare the pad to the Renderer on the next input. (The renderer may be new.)
        ResetButtonValues();          // Start with no inputs pressed
        {
            std::lock_guard lock(g_gamepadQueueMutex);
            g_gamepadQueue.clear();  // Clear any queued events from a previous session.
        }

        auto inputEventSource = RE::BSInputDeviceManager::GetSingleton();
        if (inputEventSource) {
            inputEventSource->AddEventSink(GamepadEventListener::GetSingleton());  // Begin receiving raw input events.
            logger::info("GamepadEventListener registered with BSInputDeviceManager");
        } else {
            logger::error("Failed to register GamepadEventListener: BSInputDeviceManager is null");
        }
    }

    // Processes waiting events
    void ProcessEvents() {
        if (!g_ultralightThreadExecutor) {
            return;
        }

        // Swap the queue out under the lock so it's held only briefly. Then operate on our local copy.
        std::vector<GamepadQueuedEvent> gamepadEvents;
        {
            std::lock_guard lock(g_gamepadQueueMutex);
            if (g_gamepadQueue.empty()) {
                // Nothing is queued.
                return;
            }
            // Transfer all queued events to the local queue.
            gamepadEvents.swap(g_gamepadQueue);
        }

        // Renderer calls must run on the Ultralight thread, so post the firing there.
        g_ultralightThreadExecutor->submit([events = std::move(gamepadEvents)]() {
            if (!Core::renderer) return;  // renderer was torn down between queueing and running.
            EnsureGamepadRegistered();
            for (const auto& event : events) {
                FireGamepadQueuedEvent(event);
            }
        });
    }

    void Shutdown() {
        auto inputEventSource = RE::BSInputDeviceManager::GetSingleton();
        if (inputEventSource) {
            inputEventSource->RemoveEventSink(GamepadEventListener::GetSingleton());  // stop receiving input events.
            logger::debug("GamepadEventListener removed from BSInputDeviceManager");
        }

        {
            std::lock_guard lock(g_gamepadQueueMutex);
            g_gamepadQueue.clear();  // Clear any unsent events.
        }
        g_gamepadRegistered = false;  // Allows a new session to redeclare the gamepad to the new renderer.
        g_ultralightThreadExecutor = nullptr;
    }
}
