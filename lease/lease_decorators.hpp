// lease_decorators.hpp — Layer 2: optional decorator layers + contract probe.
//
// Internal header: include via lease.hpp only.
//
// ============================================================================
// *** EXTENSION POINT ***
// ============================================================================
// This file is where new decorators are added. A decorator is a small
// compile-time policy that wraps the layer below it:
//
//     template <typename Inner, typename Policy>
//     class my_impl : public Inner {
//       ...
//     };
//
//     struct my_decorator {
//         template <typename Inner, typename Policy>
//         using apply = my_impl<Inner, Policy>;
//     };
//
// Users then opt in with make_rw<my_decorator>(object). A decorator may add
// public API (tests/access_tests.cpp has a full demo audit decorator) or
// change behavior by overriding the protected referent seam
// (mutable_object / const_object / invalidate).
// The optional write-expression seam (mutable_write_object) is called by
// write expressions to resolve the referent — override it to observe/update
// state for each write expression.
//
// Decorator state ownership / lifetime:
//   Decorator members are per-proxy-instance value members — NOT shared
//   across proxies, NOT reference-counted, NOT heap-allocated separately.
//
//   Same-type operations follow normal C++ value semantics:
//     ro copy             → state COPIED (snapshot at copy time)
//     proxy move          → state MOVED (source invalidated)
//
//   Capability-shape changes construct FRESH target-side state:
//     borrow_ro / downgrade → fresh ro-side decorator chain
//     enable_shared         → fresh shared-recipe decorator chain
//   Each target is constructed from (object, control), NOT copied or moved
//   from the source chain. enable_shared remains rw-side, but changes the
//   concrete recipe and therefore follows the same rule.
//
//   This is intentional: decorator state may not have equivalent meaning
//   after a policy or recipe change. A preservation seam would create a
//   cartesian product of conversion paths and, worse, make preservation
//   depend on which decorators happen to support it. Instead, decorator
//   state belongs to one concrete capability value. Lineage-wide state
//   (refcount, exclusivity, provenance) lives entirely in lineage_control +
//   lineage_root_slot, never in decorator members.
//
//   `mutable` members are allowed (for observation counters on const ro
//   proxies), but write-path overrides (mutable_write_object) only fire on
//   rw through write_arrow — never on ro.
//
// Every decorator MUST satisfy the contract checked by decorator_probe at the
// bottom of this file:
//   1. expose the apply seam;
//   2. declare value_type matching the referent type and policy_type;
//   3. be nothrow constructible from (value_type*, lineage_control*);
//   4. ro_tag: be nothrow copy-constructible; rw_tag: no copy required;
//   5. be nothrow move-constructible;
//   6. never be assignable (authority must not be duplicated by assignment).
// The final composed type is also checked at materialization time
// (materialize_list static_asserts) — that is the real enforcement gate.
// `probe` below is the minimal valid example — copy it to start a new
// decorator, then add a decorator_probe static_assert for the new type.
// ============================================================================

#ifndef LEASE_DECORATORS_HPP
#define LEASE_DECORATORS_HPP

#include <type_traits>
#include <utility>

#include "lease_storage.hpp"
#include "lease_type_list.hpp"

namespace lease {
namespace access {

// ============================================================
// Optional shared decorator.
//
// `shared` is the explicit opt-in for the multi-threaded mode. Explicit
// beats implicit: a bare recipe (no `shared`) is single-threaded and free —
// no atomics, no heap control block, no registry entry. Listing `shared`
// arms the participant-count era: counting is atomic and write expressions
// check exclusivity (count == 1) in Debug.
//
// Sharing rule (compile-time enforced):
//   an exclusive_access without `shared` may be passed to enable_shared()
//   to obtain a shared one; calling enable_shared() on an already-shared
//   proxy triggers a static_assert, and no reverse conversion exists — it
//   can never silently weaken back.
// ============================================================
        template <typename Inner, typename Policy>
        class shared_impl;

// Shared read decorator. Pure recipe marker — does NOT participate
// in refcounting. The lineage_root_slot base handles acquire/release.
// `shared`'s presence in the recipe drives the shared path (control block
// exists, write_arrow checks exclusivity in Debug).
        template <typename Inner>
        class shared_impl<Inner, ro_tag> : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = ro_tag;
            using Inner::Inner;

            shared_impl(const shared_impl&) noexcept = default;
            shared_impl(shared_impl&&) noexcept = default;
            shared_impl& operator=(const shared_impl&) = delete;
            shared_impl& operator=(shared_impl&&) = delete;
        };

// rw-side `shared` is a pure recipe marker: an exclusive proxy's write
// expressions check refcount for exclusivity. Its presence in the
// recipe drives the shared path (control block exists).
        template <typename Inner>
        class shared_impl<Inner, rw_tag> : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = rw_tag;
            using Inner::Inner;
        };

        struct shared {
            template <typename Inner, typename Policy>
            using apply = shared_impl<Inner, Policy>;
        };

// enable_shared() target recipe: the source recipe plus `shared` at the head.
        template <typename L>
        struct shared_list {
            using type = typename prepend<shared, L>::type;
        };

// ============================================================
// indexed decorator: conditional operator[] forwarding.
//
// Adds operator[] to shared_access<T> when T supports indexing.
// Detection priority:
//   1. T::key_type exists → map-like, forwards to at(key_type)
//      (map::operator[] is non-const and inserts; at() is the
//       const-safe lookup that throws instead of silently inserting)
//   2. T supports operator[](size_t) → sequence-like, direct forward
//   3. Neither → operator[] does not exist (SFINAE, zero cost)
//
// Shallow: forwards to T's own indexing, returning T's own reference
// type. Does not wrap elements in capabilities.
//
// ro_tag only: participant count ensures no writer is active, so const
// element access is safe. For rw_tag, use rw->operator[](key) through
// write_arrow, which checks exclusivity in Debug.
// ============================================================
        namespace detail {

            // Priority 1: T has key_type AND at(key_type) (map-like containers).
            // Requiring at() avoids false positives on set-like containers that
            // have key_type but no at() (e.g. std::set).
            template <typename T, typename = void>
            struct map_key { static constexpr bool value = false; };
            template <typename T>
            struct map_key<T, void_t<
                typename T::key_type,
                decltype(std::declval<const T&>().at(std::declval<typename T::key_type>()))
            >> {
                static constexpr bool value = true;
                using type = typename T::key_type;
            };

            // Priority 2: T supports operator[](size_t) (sequence containers).
            template <typename T, typename = void>
            struct has_index_access : std::false_type {};
            template <typename T>
            struct has_index_access<T, void_t<
                decltype(std::declval<const T&>()[std::size_t{}])
            >> : std::true_type {};

            // Combined: does T support any form of indexing?
            template <typename T>
            struct is_indexable {
                static constexpr bool value =
                    map_key<T>::value || has_index_access<T>::value;
            };

            // Key type resolver: map_key::type if available, else size_t.
            template <typename T, bool IsMap = map_key<T>::value>
            struct index_key { using type = std::size_t; };
            template <typename T>
            struct index_key<T, true> { using type = typename map_key<T>::type; };

            // Tag-dispatch helper: index a const T by key.
            // Map path forwards to at() (const-safe, throws on missing key);
            // sequence path forwards to operator[] (const overload exists).
            template <typename T, typename Key>
            auto do_index(const T& obj, Key k, std::true_type /*is_map*/)
                -> decltype(obj.at(k)) {
                return obj.at(k);
            }

            template <typename T, typename Key>
            auto do_index(const T& obj, Key k, std::false_type /*is_map*/)
                -> decltype(obj[k]) {
                return obj[k];
            }

        } // namespace detail

        // Primary template — undefined. Specialized below.
        template <typename Inner, typename Policy, typename = void>
        class indexed_impl;

        // ro_tag + indexable T: adds operator[].
        template <typename Inner>
        class indexed_impl<Inner, ro_tag,
                std::enable_if_t<detail::is_indexable<typename Inner::value_type>::value>>
            : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = ro_tag;
            using key_type = typename detail::index_key<value_type>::type;

            using Inner::Inner;
            indexed_impl(const indexed_impl&) noexcept = default;
            indexed_impl(indexed_impl&&) noexcept = default;
            indexed_impl& operator=(const indexed_impl&) = delete;
            indexed_impl& operator=(indexed_impl&&) = delete;

            // Single operator[] — tag dispatch picks at() for maps (const-safe)
            // and operator[] for sequences (const overload exists).
            auto operator[](key_type k) const
                -> decltype(detail::do_index(
                        std::declval<const value_type&>(),
                        std::declval<key_type>(),
                        std::integral_constant<bool, detail::map_key<value_type>::value>{})) {
                const value_type* obj = this->const_object();
                if (!obj) {
                    contract_violation("indexing through an empty shared_access");
                }
                return detail::do_index(*obj, k,
                        std::integral_constant<bool, detail::map_key<value_type>::value>{});
            }
        };

        // ro_tag + non-indexable T: pass-through, no operator[].
        template <typename Inner>
        class indexed_impl<Inner, ro_tag,
                std::enable_if_t<!detail::is_indexable<typename Inner::value_type>::value>>
            : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = ro_tag;
            using Inner::Inner;
            indexed_impl(const indexed_impl&) noexcept = default;
            indexed_impl(indexed_impl&&) noexcept = default;
            indexed_impl& operator=(const indexed_impl&) = delete;
            indexed_impl& operator=(indexed_impl&&) = delete;
        };

        // rw_tag: always pass-through. For indexed write access, use
        // rw->operator[](key) through write_arrow (writer exclusion).
        template <typename Inner, typename V>
        class indexed_impl<Inner, rw_tag, V> : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = rw_tag;
            using Inner::Inner;
            indexed_impl(const indexed_impl&) noexcept = default;
            indexed_impl(indexed_impl&&) noexcept = default;
            indexed_impl& operator=(const indexed_impl&) = delete;
            indexed_impl& operator=(indexed_impl&&) = delete;
        };

        struct indexed {
            template <typename Inner, typename Policy>
            using apply = indexed_impl<Inner, Policy>;
        };

// ============================================================
// Decorator contract probe.
//
// Compile-time check every decorator must pass. Adding a decorator to this
// file without a passing decorator_probe static_assert is a build error.
// ============================================================
        namespace detail {

            // 1. The recipe seam must exist: D::template apply<Inner, Policy>.
            template <typename D, typename Inner, typename Policy, typename = void>
            struct has_apply : std::false_type {};
            template <typename D, typename Inner, typename Policy>
            struct has_apply<D, Inner, Policy,
                             void_t<typename D::template apply<Inner, Policy>>>
                    : std::true_type {};

            // 2. The materialized impl must declare value_type.
            template <typename Impl, typename = void>
            struct has_value_type : std::false_type {};
            template <typename Impl>
            struct has_value_type<Impl, void_t<typename Impl::value_type>>
                    : std::true_type {};

            // 3. The impl must be constructible from (value_type*, lineage_control*).
            template <typename Impl, typename = void>
            struct ctor_ok : std::false_type {};
            template <typename Impl>
            struct ctor_ok<Impl,
                           void_t<decltype(Impl(
                                   std::declval<typename Impl::value_type*>(),
                                   std::declval<lineage_control*>()))>>
                    : std::true_type {};

        } // namespace detail

        template <typename Decorator, typename T, typename Policy>
        struct decorator_probe {
            static_assert(detail::has_apply<Decorator, object_storage<T>, Policy>::value,
                          "decorator must expose template<typename Inner, typename Policy> "
                          "using apply = ...;");
            using Impl = typename Decorator::template apply<object_storage<T>, Policy>;

            static_assert(detail::has_value_type<Impl>::value,
                          "decorator impl must declare value_type");
            static_assert(std::is_same<typename Impl::value_type, T>::value,
                          "decorator impl value_type must match the referent type");
            static_assert(detail::ctor_ok<Impl>::value,
                          "decorator impl must be constructible from "
                          "(value_type*, lineage_control*)");
            static_assert(
                std::is_nothrow_constructible<Impl,
                    typename Impl::value_type*, lineage_control*>::value,
                "decorator capability construction must be nothrow "
                "(topology operations are unconditional noexcept)");
            static_assert(
                std::is_same<Policy, rw_tag>::value ||
                std::is_nothrow_copy_constructible<Impl>::value,
                "read-capability decorator impl must be nothrow copy-constructible "
                "(borrow_ro/downgrade/ro copy are unconditional noexcept)");
            static_assert(std::is_nothrow_move_constructible<Impl>::value,
                          "decorator impl must be nothrow move-constructible "
                          "(proxy move is unconditional noexcept)");
            static_assert(!std::is_copy_assignable<Impl>::value &&
                                  !std::is_move_assignable<Impl>::value,
                          "decorator impl must never be assignable");

            static constexpr bool value = true;
        };

// ============================================================
// Minimal valid decorator — the template to copy for new extensions.
// It adds nothing; it only demonstrates the full contract shape.
// ============================================================
        template <typename Inner, typename Policy>
        class probe_impl : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = Policy;

            // 3. Construct from (value_type*, lineage_control*), forwarding down.
            probe_impl(value_type* object, lineage_control* control) noexcept
                    : Inner(object, control) {}

            // 4. Copies must work (derived readers are copies).
            probe_impl(const probe_impl&) noexcept = default;
            probe_impl(probe_impl&&) noexcept = default;

            // 5. Assignment is forbidden (authority is never duplicated).
            probe_impl& operator=(const probe_impl&) = delete;
            probe_impl& operator=(probe_impl&&) = delete;

            // Referent access goes through Inner's protected seam:
            //   mutable_object() / const_object() / valid_object() / invalidate()
            // Override them to observe or gate access (see the demo audit
            // decorator in tests/access_tests.cpp for a worked example).
        };

        struct probe {
            template <typename Inner, typename Policy>
            using apply = probe_impl<Inner, Policy>;
        };

// ---- Built-in decorator self-checks (extension point gate) -----------------
        static_assert(decorator_probe<shared, int, ro_tag>::value,
                      "shared must satisfy the decorator contract");
        static_assert(decorator_probe<probe, int, ro_tag>::value,
                      "probe must satisfy the decorator contract");
        static_assert(decorator_probe<indexed, int, ro_tag>::value,
                      "indexed must satisfy the decorator contract (non-indexable T)");
        static_assert(decorator_probe<indexed, int, rw_tag>::value,
                      "indexed must satisfy the decorator contract (rw_tag pass-through)");

} // namespace access
} // namespace lease

#endif // LEASE_DECORATORS_HPP
