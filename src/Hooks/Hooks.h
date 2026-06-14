#pragma once

#include "Utils/TraitUtils.h"

namespace Hooks {
    template <typename T>
    concept HookDefinition =
        FunctionTraits<typename T::FuncDefinition>::IsFunction &&
        std::same_as<typename FunctionTraits<typename T::FuncDefinition>::ReturnType, void> && requires {
            { T::Id } -> std::same_as<const REL::RelocationID&>;
            { T::Offset } -> std::same_as<const REL::VariantOffset&>;
        };

    template <HookDefinition THook>
    REL::Relocation<typename THook::FuncDefinition> Install(typename THook::FuncDefinition* func) {
        REL::Relocation<> hook(THook::Id, THook::Offset);
        return REL::Relocation<typename THook::FuncDefinition>(
            SKSE::GetTrampoline().write_call<5>(hook.address(), func));
    }
}