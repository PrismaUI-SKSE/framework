#pragma once

#include "Utils/TraitUtils.h"
#include "MinHook.h"

namespace Hooks {
    enum class HookType {
        WriteCall = 1,
        Direct,
    };

    template <typename T>
    concept HookDefinition =
        FunctionTraits<typename T::FuncDefinition>::IsFunction &&
        std::same_as<typename FunctionTraits<typename T::FuncDefinition>::ReturnType, void> && requires {
            { T::Id } -> std::same_as<const REL::RelocationID&>;
            { T::Offset } -> std::same_as<const REL::VariantOffset&>;
            { T::Type } -> std::same_as<const HookType&>;
        };

    template <HookDefinition THook>
    REL::Relocation<typename THook::FuncDefinition> Install(typename THook::FuncDefinition* func) {
        auto targetAddress = REL::Relocation<>(THook::Id, THook::Offset).address();
        if constexpr (THook::Type == HookType::WriteCall) {
            return REL::Relocation<typename THook::FuncDefinition>(SKSE::GetTrampoline().write_call<5>(targetAddress, func));
        } else if constexpr (THook::Type == HookType::Direct) {
            auto status = MH_Initialize();
            if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
                logger::critical("MinHook init failed");
                std::terminate();
            }

            LPVOID origAddress;
            if (MH_CreateHook(reinterpret_cast<LPVOID>(targetAddress), func, &origAddress) != MH_OK) {
                logger::critical("Failed to create hook");
                std::terminate();
            }

            if (MH_EnableHook(reinterpret_cast<LPVOID>(targetAddress)) != MH_OK) {
                logger::critical("Failed to enable hook");
                std::terminate();
            }

            return REL::Relocation<typename THook::FuncDefinition>(reinterpret_cast<uintptr_t>(origAddress));
        } else {
            static_assert(!sizeof(THook::Type), "Unsupported hook type");
            return 0;
        }
    }
}