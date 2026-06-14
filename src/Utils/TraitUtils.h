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

template <class Tuple, class T>
struct TupleAppend;

template <class... Ts, class T>
struct TupleAppend<std::tuple<Ts...>, T> {
    using Type = std::tuple<Ts..., T>;
};

template <class Tuple, class T>
using TupleAppendT = TupleAppend<Tuple, T>::Type;