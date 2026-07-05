#pragma once

#include <type_traits>

template <class TAction, class TNext>
struct CallPipeline {
public:
    using Action = std::decay_t<TAction>;
    using Next = std::decay_t<TNext>;

    CallPipeline(TAction&& action, TNext&& next) :
        _action(std::forward<TAction>(action)), _next(std::forward<TNext>(next)) {
    }

    template <class TWrap>
    auto Wrap(TWrap&& wrap) && {
        return CallPipeline<TWrap, CallPipeline&&>(std::forward<TWrap>(wrap), std::move(*this));
    }

    template <class... TArgs>
    requires (std::is_invocable_v<Action&, Next&, TArgs...>)
    auto operator()(TArgs... args) {
        return std::invoke(_action, _next, std::forward<TArgs>(args)...);
    }

private:
    Action _action;
    Next _next;
};

template <class TAction>
struct CallPipelineStart {
public:
    using Action = std::decay_t<TAction>;

    CallPipelineStart(TAction&& action) :
        _action(std::forward<TAction>(action)) {
    }

    template <class TWrap>
    auto Wrap(TWrap&& wrap) && {
        return CallPipeline<TWrap, Action&&>(std::forward<TWrap>(wrap), std::move(_action));
    }

    template <class... TArgs>
    requires (std::is_invocable_v<Action&, TArgs...>)
    auto operator()(TArgs... args) {
        return std::invoke(_action, std::forward<TArgs>(args)...);
    }

private:
    Action _action;
};

template <class TAction>
CallPipelineStart<TAction> StartCallPipeline(TAction&& action) {
    return CallPipelineStart<TAction>(std::forward<TAction>(action));
}