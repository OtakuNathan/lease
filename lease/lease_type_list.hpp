// lease_type_list.hpp — compile-time type-list IR and recipe utilities.
//
// Borrowed from dynabridge/type_list.h (Nathan's flat-inheritance design):
// instead of a recursive linked-list layout, the list is a set of indexed
// base leaves, so element_at<I> resolves by overload resolution in O(1)
// rather than by template recursion. same philosophy as flat_storage vs.
// MSVC's chained std::tuple.
//
// Internal header: include via lease_decorators.hpp only.

#ifndef LEASE_TYPE_LIST_HPP
#define LEASE_TYPE_LIST_HPP

#include <cstddef>
#include <type_traits>
#include <utility>

namespace lease {
namespace access {

        template <size_t I, typename T>
        struct type_list_leaf { using type = T; };

        template <typename Is, typename... Ts>
        struct type_list_base;

        template <size_t... idx, typename... Ts>
        struct type_list_base<std::index_sequence<idx...>, Ts...>
                : type_list_leaf<idx, Ts>... {};

        template <typename... Ts>
        struct type_list
                : type_list_base<std::index_sequence_for<Ts...>, Ts...> {
            template <size_t I, typename T>
            static typename type_list_leaf<I, T>::type
            element_at(type_list_leaf<I, T>) noexcept;
        };

        template <typename T>
        struct type_list_size;

        template <typename... Ts>
        struct type_list_size<type_list<Ts...>>
                : std::integral_constant<size_t, sizeof...(Ts)> {};

        template <size_t I, typename T>
        struct element_at;

        template <size_t I, typename... Ts>
        struct element_at<I, type_list<Ts...>> {
            static_assert(I < sizeof...(Ts), "index out of range");
            using type = decltype(
                    type_list<Ts...>::template element_at<I>(
                            std::declval<type_list<Ts...>>()));
        };

        template <size_t I, typename T>
        using element_at_t = typename element_at<I, T>::type;

        // prepend
        template <typename T, typename TL>
        struct prepend;

        template <typename T, typename... Ts>
        struct prepend<T, type_list<Ts...>> {
            using type = type_list<T, Ts...>;
        };

        template <typename T, typename TL>
        using prepend_t = typename prepend<T, TL>::type;

        // contains (recursive; needed by the recipe machinery)
        template <typename Needle, typename List>
        struct contains;

        template <typename Needle>
        struct contains<Needle, type_list<>> : std::false_type {};

        template <typename Needle, typename Head, typename... Tail>
        struct contains<Needle, type_list<Head, Tail...>>
                : std::integral_constant<
                        bool,
                        std::is_same<Needle, Head>::value ||
                        contains<Needle, type_list<Tail...>>::value> {};

} // namespace access
} // namespace lease

#endif // LEASE_TYPE_LIST_HPP
