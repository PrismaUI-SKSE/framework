#pragma once

#include "Hooks.h"
#include "REL/REL.h"
#include "Utils/CallPipeline.h"

namespace Hooks {
    namespace Detail {
        template <HookDefinition THook, class THandler, class TArgs>
        struct HookFunc {
            static_assert(false, "Check THandler, it seems it doesn't satisfy hook parameters");
        };

        template <HookDefinition THook, class THandler, class... TArgs>
            requires(std::is_invocable_v<const std::decay_t<THandler>&,
                                         const REL::Relocation<typename THook::FuncDefinition>&, TArgs...>)
        struct HookFunc<THook, THandler, std::tuple<TArgs...>> {
        public:
            static void Invoke(TArgs... args) { std::invoke(CallPipeline, std::forward<TArgs>(args)...); }

            static inline CallPipeline<THandler, REL::Relocation<typename THook::FuncDefinition>&&> CallPipeline;
        };
    }

    template <HookDefinition THook>
    class HookInstaller {
    public:
        template <class THandler>
        static void Install(THandler&& handler) {
            if (IsInstalled) {
                logger::error("Hook {} is already installed", typeid(THook).name());
                throw std::runtime_error(std::format("Hook {} is already installed", typeid(THook).name()));
            }

            using HookFunc =
                Detail::HookFunc<THook, THandler, typename FunctionTraits<typename THook::FuncDefinition>::Parameters>;
            auto originalFunc = Hooks::Install<THook>(HookFunc::Invoke);
            HookFunc::CallPipeline = StartCallPipeline(originalFunc).Wrap(std::forward<THandler>(handler));
            IsInstalled = true;
        }

    private:
        static inline bool IsInstalled = false;
    };
}
