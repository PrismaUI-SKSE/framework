#pragma once

#include <optional>

#include "Hooks.h"
#include "REL/REL.h"

namespace Hooks {
    namespace Detail {
        template <HookDefinition, typename TPreHandlers, typename TPostHandlers>
        struct HookHandlers {
        public:
            static inline std::optional<TPreHandlers> PreHandlers;
            static inline std::optional<TPostHandlers> PostHandlers;
        };

        template <HookDefinition THook, typename TPreHandlers, typename TPostHandlers, typename TArgs>
        struct HookFunc;

        template <HookDefinition THook, typename TPreHandlers, typename TPostHandlers, typename... TArgs>
        struct HookFunc<THook, TPreHandlers, TPostHandlers, std::tuple<TArgs...>> {
        public:
            static void Invoke(TArgs... args) {
                using HandlerStorage = HookHandlers<THook, TPreHandlers, TPostHandlers>;

                if constexpr (std::tuple_size_v<TPreHandlers> > 0) {
                    std::apply([&](auto&... handlers) { (std::invoke(handlers, args...), ...); },
                               *HandlerStorage::PreHandlers);
                }

                OriginalFunc(args...);

                if constexpr (std::tuple_size_v<TPostHandlers> > 0) {
                    std::apply([&](auto&... handlers) { (std::invoke(handlers, args...), ...); },
                               *HandlerStorage::PostHandlers);
                }
            }

            static inline REL::Relocation<typename THook::FuncDefinition> OriginalFunc;
        };
    }

    template <HookDefinition THook, typename TPreHandlers = std::tuple<>, typename TPostHandlers = std::tuple<>>
    class HookInstaller {
    private:
        using HookHandlers = Detail::HookHandlers<THook, TPreHandlers, TPostHandlers>;

        template <HookDefinition A, typename B, typename C>
        friend class HookInstaller;

    public:
        template <typename THandler>
        HookInstaller<THook, TupleAppendT<TPreHandlers, THandler>, TPostHandlers> AddPreHandler(THandler&& handler) {
            return {
                std::tuple_cat(std::move(*HookHandlers::PreHandlers), std::tuple{std::forward<THandler>(handler)}),
                std::move(*HookHandlers::PostHandlers),
            };
        }

        template <typename THandler>
        HookInstaller<THook, TPreHandlers, TupleAppendT<TPostHandlers, THandler>> AddPostHandler(THandler&& handler) {
            return {
                std::move(*HookHandlers::PreHandlers),
                std::tuple_cat(std::move(*HookHandlers::PostHandlers), std::tuple{std::forward<THandler>(handler)}),
            };
        }

        void Install() && {
            using HookFunc = Detail::HookFunc<THook, TPreHandlers, TPostHandlers,
                                              typename FunctionTraits<typename THook::FuncDefinition>::Parameters>;
            HookFunc::OriginalFunc = Hooks::Install<THook>(HookFunc::Invoke);
        }

        static HookInstaller<THook> Create() {
            static_assert(std::same_as<TPreHandlers, std::tuple<>> && std::same_as<TPostHandlers, std::tuple<>>,
                          "Create can be called only on default HookInstaller type");
            return {std::tuple(), std::tuple()};
        }

    private:
        HookInstaller(TPreHandlers&& preHandlers, TPostHandlers&& postHandlers) {
            HookHandlers::PreHandlers.emplace(std::move(preHandlers));
            HookHandlers::PostHandlers.emplace(std::move(postHandlers));
        }
    };
}
