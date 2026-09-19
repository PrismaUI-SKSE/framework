#pragma once

#include <functional>

namespace PrismaUI::MainThreadQueue {
    void Initialize();
    void Shutdown();

    void Post(std::function<void()> task);
    void Drain();
    void Clear();
}
