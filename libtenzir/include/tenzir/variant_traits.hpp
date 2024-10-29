//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2024 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tenzir/concepts.hpp"
#include "tenzir/detail/assert.hpp"
#include "tenzir/detail/overload.hpp"

#include <caf/variant.hpp>

#include <variant>

namespace tenzir {

template <class T, class U>
constexpr auto forward_like(U&& x) noexcept -> auto&& {
  constexpr bool is_adding_const = std::is_const_v<std::remove_reference_t<T>>;
  if constexpr (std::is_lvalue_reference_v<T&&>) {
    if constexpr (is_adding_const) {
      return std::as_const(x);
    } else {
      return static_cast<U&>(x);
    }
  } else {
    if constexpr (is_adding_const) {
      return std::move(std::as_const(x));
    } else {
      return std::move(x);
    }
  }
}

template <class T, class U>
using forward_like_t = decltype(forward_like<T>(std::declval<U>()));

/// The opposite of `std::as_const`, removing `const` qualifiers.
template <class T>
auto as_mutable(T& x) -> T& {
  return x;
}
template <class T>
auto as_mutable(const T& x) -> T& {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  return const_cast<T&>(x);
}
template <class T>
auto as_mutable(T&& x) -> T&& {
  return std::forward<T>(x);
}
template <class T>
auto as_mutable(const T&& x) -> T&& {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  return const_cast<T&&>(x);
}

/// Enables variant methods (like `match`) for a given type.
///
/// Implementations need to provide the following `static` members:
/// - `constexpr size_t count`
/// - `index(const T&) -> size_t`
/// - `get<size_t>(const T&) -> const U&`
///
/// The `index` function may only return indicies in `[0, count)`. The `get`
/// function is instantiated for all `[0, count)` and may assume that the
/// given index is what `index(...)` previously returned. If the original
/// object was `T&` or `T&&`, the implementation will `const_cast` the result
/// of `get(...)`.
template <concepts::unqualified T>
class variant_traits;

template <class... Ts>
class variant_traits<std::variant<Ts...>> {
public:
  static constexpr auto count = sizeof...(Ts);

  static constexpr auto index(const std::variant<Ts...>& x) -> size_t {
    return x.index();
  }

  template <size_t I>
  static constexpr auto get(const std::variant<Ts...>& x) -> decltype(auto) {
    return *std::get_if<I>(&x);
  }
};

template <class... Ts>
class variant_traits<caf::variant<Ts...>> {
public:
  static constexpr auto count = sizeof...(Ts);

  static auto index(const caf::variant<Ts...>& x) -> size_t {
    return x.index();
  }

  template <size_t I>
  static auto get(const caf::variant<Ts...>& x) -> decltype(auto) {
    using T = std::tuple_element_t<I, std::tuple<Ts...>>;
    return *caf::sum_type_access<caf::variant<Ts...>>::get_if(
      &x, caf::sum_type_token<T, I>{});
  }
};

namespace detail {

/// Dispatches to `variant_traits<V>::get` and also transfers qualifiers.
template <size_t I, class V>
constexpr auto variant_get(V&& v) -> decltype(auto) {
  static_assert(
    std::is_reference_v<
      decltype(variant_traits<std::remove_cvref_t<V>>::template get<I>(v))>);
  // We call `as_mutable` here because `forward_like` never removes `const`.
  return forward_like<V>(
    as_mutable(variant_traits<std::remove_cvref_t<V>>::template get<I>(v)));
}

template <class V, class F>
constexpr auto match_one(V&& v, F&& f) -> decltype(auto) {
  using traits = variant_traits<std::remove_cvref_t<V>>;
  using return_type = std::invoke_result_t<F, decltype(variant_get<0>(v))>;
  auto index = traits::index(std::as_const(v));
  static_assert(std::same_as<decltype(index), size_t>);
  // TODO: A switch/if-style dispatch might be more performant.
  constexpr auto table = std::invoke(
    []<size_t... Is>(std::index_sequence<Is...>) {
      return std::array{
        // Arguments are not necessarily &&-refs due to reference collapsing.
        // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
        +[](F&& f, V&& v) -> return_type {
          // We repeat ourselves here because we don't want to handle the case
          // where `void` is returned separately.
          using local_return_type = decltype(std::invoke(
            std::forward<F>(f), variant_get<Is>(std::forward<V>(v))));
          static_assert(std::same_as<local_return_type, return_type>,
                        "all cases must have the same return type");
          return std::invoke(std::forward<F>(f),
                             variant_get<Is>(std::forward<V>(v)));
        }...,
      };
    },
    std::make_index_sequence<traits::count>());
  static_assert(table.size() == traits::count);
  TENZIR_ASSERT(index < traits::count);
  return table[index](std::forward<F>(f), std::forward<V>(v)); // NOLINT
}

template <class... Xs, class F>
constexpr auto match_tuple(std::tuple<Xs...> xs, F&& f) -> decltype(auto) {
  // There are probably more performant ways to write this, but the
  // implementation below should be good enough for now.
  if constexpr (sizeof...(Xs) == 0) {
    return f();
  } else {
    auto&& x = std::get<0>(xs);
    return match_one(std::forward<decltype(x)>(x),
                     [&]<class X>(X&& x) -> decltype(auto) {
                       return match_tuple(
                         // Strip the first value from the tuple.
                         std::apply(
                           []<class... Ys>(auto&&, Ys&&... ys) {
                             return std::tuple<Ys&&...>{ys...};
                           },
                           xs),
                         // Then combine the already matched `x` with the rest.
                         [&]<class... Ys>(Ys&&... ys) -> decltype(auto) {
                           return std::forward<F>(f)(std::forward<X>(x),
                                                     std::forward<Ys>(ys)...);
                         });
                     });
  }
}

template <class V, class T>
constexpr auto variant_index = std::invoke(
  []<size_t... Is>(std::index_sequence<Is...>) {
    auto result = 0;
    auto found
      = (std::invoke([&] {
           if (std::same_as<
                 std::decay_t<decltype(variant_traits<V>::template get<Is>(
                   std::declval<V>()))>,
                 T>) {
             result = Is;
             return 1;
           }
           return 0;
         })
         + ... + 0);
    if (found == 0) {
      throw std::runtime_error{"type was not found in variant"};
    }
    if (found > 1) {
      throw std::runtime_error{"type was found multiple times in variant"};
    }
    return result;
  },
  std::make_index_sequence<variant_traits<V>::count>());

} // namespace detail

template <class V, class... Fs>
constexpr auto match(V&& v, Fs&&... fs) -> decltype(auto) {
  // We materialize a `detail::overload` below, which means copying and moving
  // the functions.
  if constexpr (caf::detail::is_specialization<std::tuple,
                                               std::remove_cvref_t<V>>::value) {
    return match_tuple(std::forward<V>(v),
                       detail::overload{std::forward<Fs>(fs)...});
  } else {
    return match_one(std::forward<V>(v),
                     detail::overload{std::forward<Fs>(fs)...});
  }
}

/// Casts a variant to the given type `T`, asserting success.
template <concepts::unqualified T, class V>
auto as(V&& v) -> forward_like_t<V, T> {
  constexpr auto index = detail::variant_index<std::remove_cvref_t<V>, T>;
  TENZIR_ASSERT(variant_traits<std::remove_cvref_t<V>>::index(v) == index);
  return detail::variant_get<index>(std::forward<V>(v));
};

// TODO: Really `V&` and `V*`?
template <concepts::unqualified T, class V>
auto try_as(V& v) -> std::remove_reference_t<forward_like_t<V, T>>* {
  constexpr auto index = detail::variant_index<std::remove_const_t<V>, T>;
  if (variant_traits<std::remove_const_t<V>>::index(v) != index) {
    return nullptr;
  }
  return &detail::variant_get<index>(v);
};
template <class T, class V>
auto try_as(V* v) -> std::remove_reference_t<forward_like_t<V, T>>* {
  if (not v) {
    return nullptr;
  }
  return try_as<T>(*v);
};

} // namespace tenzir
