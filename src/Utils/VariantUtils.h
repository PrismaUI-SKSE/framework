#pragma once

#include <variant>

template <typename... Ts>
struct Overload : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overload(Ts...) -> Overload<Ts...>;

template <class Variant, class... Visitors>
decltype(auto) Match(Variant&& variant, Visitors&&... visitors) {
    return std::visit(Overload{std::forward<Visitors>(visitors)...}, std::forward<Variant>(variant));
}