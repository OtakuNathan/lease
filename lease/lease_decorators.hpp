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
// write expressions *after* writer exclusion has been acquired — override it
// to observe/update state inside the protocol's happens-before chain.
//
// Every decorator MUST satisfy the contract checked by decorator_probe at the
// bottom of this file:
//   1. expose the apply seam;
//   2. declare value_type matching the referent type and policy_type;
//   3. be constructible from (value_type*, lineage_control*);
//   4. be copy-constructible (derived readers are copies);
//   5. never be assignable (authority must not be duplicated by assignment).
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
// Optional read lock decorator.
//
// read_lock is the explicit opt-in for the multi-threaded mode. Explicit
// beats implicit: a bare recipe (no read_lock) is single-threaded and free —
// no atomics, no heap control block, no registry entry. Listing read_lock
// arms the shared era: reader counting is atomic and writer expressions use
// CAS exclusion.
//
// Lock rule (compile-time enforced):
//   an exclusive_access without read_lock may lock() into a locked one; a
//   locked exclusive_access has no lock() member at all (SFINAE) and no
//   reverse conversion exists — it can never silently weaken back.
// ============================================================
        template <typename Inner, typename Policy>
        class read_lock_impl;

// Locked read-share decorator. Each derived copy contributes one reader;
// the last live participant after the root has gone deletes the orphaned
// control block (the read_lease contract, moved into the decorator chain).
        template <typename Inner>
        class read_lock_impl<Inner, ro_tag> : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = ro_tag;

            read_lock_impl(value_type* object, lineage_control* control) noexcept
                    : Inner(object, control), control_(control) {
                if (control_) {
                    access_counter::acquire_reader(control_->state());
                }
            }

            read_lock_impl(const read_lock_impl& rhs) noexcept
                    : Inner(rhs), control_(rhs.control_) {
                if (control_) {
                    access_counter::acquire_reader(control_->state());
                }
            }

            read_lock_impl(read_lock_impl&& rhs) noexcept
                    : Inner(std::move(rhs)),
                      control_(std::exchange(rhs.control_, nullptr)) {}

            read_lock_impl& operator=(const read_lock_impl&) = delete;
            read_lock_impl& operator=(read_lock_impl&&) = delete;

            ~read_lock_impl() noexcept {
                if (control_ && access_counter::release_reader(control_->state())) {
                    delete control_;
                }
            }

        protected:
            lineage_control* control_pointer() const noexcept { return control_; }

        private:
            lineage_control* control_;
        };

// rw-side read_lock is a pure recipe marker: an exclusive proxy never counts
// readers itself. Its presence in the recipe drives the locked writer path.
        template <typename Inner>
        class read_lock_impl<Inner, rw_tag> : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = rw_tag;
            using Inner::Inner;
        };

        struct read_lock {
            template <typename Inner, typename Policy>
            using apply = read_lock_impl<Inner, Policy>;
        };

// lock() target recipe: the source recipe plus read_lock at the head.
        template <typename L>
        struct lock_list {
            using type = typename prepend<read_lock, L>::type;
        };

// ============================================================
// Decorator contract probe.
//
// Compile-time check every decorator must pass. Adding a decorator to this
// file without a passing decorator_probe static_assert is a build error.
// ============================================================
        namespace detail {

            template <typename...>
            using void_t = void;  // C++14 void_t

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
            static_assert(std::is_copy_constructible<Impl>::value,
                          "decorator impl must be copy-constructible "
                          "(derived readers are copies)");
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
        static_assert(decorator_probe<read_lock, int, ro_tag>::value,
                      "read_lock must satisfy the decorator contract");
        static_assert(decorator_probe<probe, int, ro_tag>::value,
                      "probe must satisfy the decorator contract");

} // namespace access
} // namespace lease

#endif // LEASE_DECORATORS_HPP
