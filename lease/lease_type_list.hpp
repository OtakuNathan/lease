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

        // C++14 void_t — used by decorator probes and trait detectors
        // throughout the library. Defined here (the lowest layer) so every
        // header that includes type_list can use it without a fragile
        // ordering dependency.
        template <typename...>
        using void_t = void;

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

        // ---- dedup ------------------------------------------------------------
        // Remove duplicate types from a type_list, keeping the first occurrence
        // and discarding later repeats. User intent lives in declaration order:
        // the first mention wins, so an explicit decorator always overrides an
        // auto-injected one, and accidental repeats like <indexed, indexed>
        // collapse to a single layer.
        //
        // Implementation: fold left, threading a "seen so far" accumulator.
        // Each candidate is appended only if it is not already in `seen`.

        // Helper: append T to List only if T is not already present.
        template <typename T, typename List, bool Already = contains<T, List>::value>
        struct append_if_new {
            using type = List;  // already present — drop the duplicate
        };
        template <typename T, typename List>
        struct append_if_new<T, List, false> {
            using type = typename prepend<T, List>::type;  // not seen — keep
        };

        // We build the result in reverse (prepend), then reverse at the end.
        // Reverse of a type_list.
        template <typename List, typename Acc = type_list<>>
        struct reverse;
        template <typename Acc>
        struct reverse<type_list<>, Acc> { using type = Acc; };
        template <typename Head, typename... Tail, typename Acc>
        struct reverse<type_list<Head, Tail...>, Acc>
            : reverse<type_list<Tail...>, typename prepend<Head, Acc>::type> {};

        // Dedup core: fold over the input, threading `seen`.
        template <typename Input, typename Seen>
        struct dedup_impl;
        template <typename Seen>
        struct dedup_impl<type_list<>, Seen> { using type = Seen; };
        template <typename Head, typename... Tail, typename Seen>
        struct dedup_impl<type_list<Head, Tail...>, Seen>
            : dedup_impl<type_list<Tail...>,
                         typename append_if_new<Head, Seen>::type> {};

        // Public entry point: dedup a type_list, preserving first-occurrence order.
        template <typename List>
        struct dedup;
        template <typename... Ts>
        struct dedup<type_list<Ts...>> {
            // dedup_impl prepends, so the result is reversed relative to input.
            // Reverse back to restore declaration order.
            using type = typename reverse<typename dedup_impl<
                type_list<Ts...>, type_list<>>::type>::type;
        };

        template <typename List>
        using dedup_t = typename dedup<List>::type;

} // namespace access
} // namespace lease

#endif // LEASE_TYPE_LIST_HPP
