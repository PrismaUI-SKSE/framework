#include "Hooks.h"

namespace Hooks {
    template <typename T> uintptr_t __install(REL::Relocation<T> hook, void* func) {
        auto& trampoline = SKSE::GetTrampoline();
        return trampoline.write_call<5>(hook.address(), func);
    }

    uintptr_t D3DInitHook::Install(D3DInitFunc* func)
    {
        REL::Relocation<uintptr_t> hook{ id, offset };
        return __install(hook, func);
    }

    uintptr_t D3DPresentHook::Install(D3DPresentFunc* func)
    {
        REL::Relocation<uint32_t> hook{ id, offset };
        return __install(hook, func);
    }

    uintptr_t UpdateHook::Install(UpdateFunc* func)
    {
        REL::Relocation<uint32_t> hook{ id, offset };
        return __install(hook, func);
    }
}
