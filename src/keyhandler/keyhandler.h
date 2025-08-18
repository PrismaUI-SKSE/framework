#pragma once

#include <functional>
#include <map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <cstdint>

using KeyCallback = std::function<void()>;
using KeyHandlerEvent = uint64_t;

inline constexpr KeyHandlerEvent INVALID_REGISTRATION_HANDLE = 0;

enum class KeyEventType : uint8_t
{
    KEY_DOWN,
    KEY_UP
};

struct CallbackInfo
{
    uint32_t     key = 0;
    KeyEventType type = KeyEventType::KEY_DOWN;
};

class KeyHandler : public RE::BSTEventSink<RE::InputEvent*>
{
public:
    static KeyHandler* GetSingleton();
    static void RegisterSink();

    [[nodiscard]] KeyHandlerEvent Register(uint32_t dxScanCode, KeyEventType eventType, KeyCallback callback);

    void Unregister(KeyHandlerEvent handle);

private:
    KeyHandler() = default;
    ~KeyHandler() override = default;

    KeyHandler(const KeyHandler&) = delete;
    KeyHandler(KeyHandler&&) = delete;
    KeyHandler& operator=(const KeyHandler&) = delete;
    KeyHandler& operator=(KeyHandler&&) = delete;

    RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_eventList, RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

    struct KeyCallbacks
    {
        std::map<KeyHandlerEvent, KeyCallback> down;
        std::map<KeyHandlerEvent, KeyCallback> up;
    };

    std::map<uint32_t, KeyCallbacks> _registeredCallbacks;
    std::map<KeyHandlerEvent, CallbackInfo> _handleMap;

    std::atomic<KeyHandlerEvent> _nextHandle = INVALID_REGISTRATION_HANDLE + 1;

    std::shared_mutex _mutex;
};

//auto keyHandler = KeyHandler::GetSingleton();
//
//const uint32_t G_KEY = 0x22; // G
//
//KeyHandlerEvent G_downEventHandler = keyHandler->Register(G_KEY, KeyEventType::KEY_DOWN, [&]() {
//    logger::info("[Callback 1] G was pressed!");
//});
// 
//keyHandler->Unregister(G_downEventHandler);