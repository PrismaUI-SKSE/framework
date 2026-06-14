#pragma once

#include <tuple>

template <class Signature>
struct FunctionTraits {
    static constexpr bool IsFunction = false;
};

template <class R, class... Args>
struct FunctionTraits<R(Args...)> {
    using ReturnType = R;
    using Parameters = std::tuple<Args...>;

    static constexpr bool IsFunction = true;
};