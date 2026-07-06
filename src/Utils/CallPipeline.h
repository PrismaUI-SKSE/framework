#pragma once

#include <type_traits>

template <class TAction, class TNext>
struct CallPipeline {
public:
    using Action = std::decay_t<TAction>;
    using Next = std::decay_t<TNext>;

    CallPipeline() = default;

    CallPipeline(const CallPipeline&) = default;

    CallPipeline(CallPipeline&&) = default;

    CallPipeline(TAction&& action, TNext&& next) :
        _action(std::forward<TAction>(action)), _next(std::forward<TNext>(next)) {
    }

    template <class TWrap>
    auto Wrap(TWrap&& wrap) && {
        return CallPipeline<TWrap, CallPipeline&&>(std::forward<TWrap>(wrap), std::move(*this));
    }

    template <class... TArgs>
    requires (std::is_invocable_v<const Action&, const Next&, TArgs...>)
    auto operator()(TArgs... args) const {
        return std::invoke(_action, _next, std::forward<TArgs>(args)...);
    }

    CallPipeline& operator=(const CallPipeline& other) = default;

    CallPipeline& operator=(CallPipeline&& other) = default;

private:
    Action _action;
    Next _next;
};

template <class TAction>
struct CallPipelineStart {
public:
    using Action = std::decay_t<TAction>;

    CallPipelineStart() = default;

    CallPipelineStart(const CallPipelineStart&) = default;

    CallPipelineStart(CallPipelineStart&&) = default;

    CallPipelineStart(TAction&& action) :
        _action(std::forward<TAction>(action)) {
    }

    template <class TWrap>
    auto Wrap(TWrap&& wrap) && {
        return CallPipeline<TWrap, Action&&>(std::forward<TWrap>(wrap), std::move(_action));
    }

    template <class... TArgs>
    requires (std::is_invocable_v<const Action&, TArgs...>)
    auto operator()(TArgs... args) const {
        return std::invoke(_action, std::forward<TArgs>(args)...);
    }

    CallPipelineStart& operator=(const CallPipelineStart& other) = default;

    CallPipelineStart& operator=(CallPipelineStart&& other) = default;

private:
    Action _action;
};

template <class TAction>
CallPipelineStart<TAction> StartCallPipeline(TAction&& action) {
    return CallPipelineStart<TAction>(std::forward<TAction>(action));
}