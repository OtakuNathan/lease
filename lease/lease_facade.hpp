// lease_facade.hpp — Layer 3: proxy facades, factories, write_arrow.
//
// Internal header: include via lease.hpp only.
//
// This layer consumes the layers below and presents the public surface:
//   - materialize_*  : turns a recipe (type_list of decorators) into the final
//                      impl type
//   - write_arrow    : per-expression referent resolution + exclusivity check
//   - proxy<T, DecoratorList, ro_tag|rw_tag> : the shared/exclusive facades
//   - shared_access / exclusive_access aliases
//   - make_ro / make_rw factories
//
// Destructive conversions (enable_shared(), downgrade()) take no &&: like
// unique_ptr::release(), they are callable on lvalues, invalidate the source
// handle immediately, and any later use of that handle is a contract
// violation — never silent UB.

#ifndef LEASE_FACADE_HPP
#define LEASE_FACADE_HPP

#include "lease_decorators.hpp"

namespace lease {
namespace access {

// ============================================================
// Distributed materializer.
// User order is outer -> inner. Mandatory track and leaf are appended.
// ============================================================
        template <typename T, typename Policy, typename... Nodes>
        struct materialize_nodes;

        template <typename T, typename Policy>
        struct materialize_nodes<T, Policy, object_leaf> {
            using type = object_storage<T>;
        };

        template <typename T,
                typename Policy,
                typename Head,
                typename... Tail>
        struct materialize_nodes<T, Policy, Head, Tail...> {
            using inner_type = typename materialize_nodes<T, Policy, Tail...>::type;
            using type = typename Head::template apply<inner_type, Policy>;
        };

        template <typename T, typename Policy, typename List>
        struct materialize_list;

        template <typename T, typename Policy, typename List>
        struct materialize_recipe;

        template <typename T, typename Policy, typename... Nodes>
        struct materialize_recipe<T, Policy, type_list<Nodes...>> {
            using pre_dedup = std::conditional_t<
                detail::is_indexable<T>::value,
                type_list<Nodes..., indexed, track, object_leaf>,
                type_list<Nodes..., track, object_leaf>>;
            using recipe = dedup_t<pre_dedup>;
            template <typename R> struct unpack;
            template <typename... Rs> struct unpack<type_list<Rs...>> {
                using type = typename materialize_nodes<T, Policy, Rs...>::type;
            };
            using type = typename unpack<recipe>::type;
        };

        template <typename T,
                typename Policy,
                typename... Decorators>
        struct materialize_list<T, Policy, type_list<Decorators...>> {
            static_assert(!contains<track, type_list<Decorators...>>::value,
                          "track is mandatory and must not be listed explicitly");
            static_assert(!contains<object_leaf, type_list<Decorators...>>::value,
                          "object_leaf is an internal sentinel");

            using type = typename materialize_recipe<
                    T, Policy, type_list<Decorators...>>::type;

            // ---- Materialized capability contract ----
            // Verify the FINAL composed type (D1<D2<...<track<object_storage<T>>>>>)
            // satisfies lease's nothrow capability algebra. This is the real gate:
            // individual decorator_probe checks are diagnostic; this is enforcement.
            // It fires automatically for every proxy instantiation, user or built-in.
            //
            // Policy-sensitive: rw is unique/affine (move-only), ro is shareable
            // (copyable). Only require what the authority model actually needs.
            static_assert(
                std::is_nothrow_constructible<type, T*, lineage_control*>::value,
                "materialized capability must be nothrow constructible from "
                "(T*, lineage_control*) — topology operations are noexcept");
            static_assert(
                std::is_same<Policy, rw_tag>::value ||
                std::is_nothrow_copy_constructible<type>::value,
                "read capability must be nothrow copy-constructible "
                "— borrow_ro/downgrade/ro copy are unconditional noexcept");
            static_assert(
                std::is_nothrow_move_constructible<type>::value,
                "materialized capability must be nothrow move-constructible "
                "— proxy move is unconditional noexcept");
        };

        template <typename T, typename Policy, typename List>
        using materialize_t = typename materialize_list<T, Policy, List>::type;

// ============================================================
// write_arrow: resolves the referent for a write expression.
//
// In the intrusive refcount model, exclusivity is checked at construction:
// refcount == 1 means only the rw holder exists. No writer bit, no CAS
// loop, no scope object — the count IS the exclusivity check.
//
// Check order (critical for safety):
//   1. core_valid() — spent-token check via object_storage directly
//      (NOT through the decorator chain). A spent proxy has object == null.
//      This MUST happen before any decorator seam or control block access.
//   2. Exclusivity check via core_control() (root_slot's authoritative
//      pointer, NOT the decorator chain's observational pointer).
//      Always compiled — one atomic load, negligible cost.
//   3. mutable_write_object() — decorator write seam. Only called AFTER
//      admission succeeds, so decorator side effects (e.g. audit counter)
//      never fire on a rejected write.
//
// The Shared template parameter: bare recipes (Shared=false) have no
// control block, so the exclusivity check is skipped. Shared recipes
// (Shared=true) always check refcount == 1.
//
// Reentrancy contract: the rw holder must not reenter — no nested write
// expression, no borrow_ro(), no downgrade(), no move — while a write
// expression (operator->, write(lambda), assign) is active. The rw holder
// is the sole thread that can create the first ro, so no external thread
// can violate this. lease replaces raw references; write lambdas must not
// capture the rw proxy by reference. Violating this is a contract breach,
// not a runtime-synchronized path.
// ============================================================
        template <typename T, typename DecoratorList, typename Policy>
        class proxy;

        template <typename T, bool Shared>
        class write_arrow {
        public:
            template <typename DL>
            write_arrow(proxy<T, DL, rw_tag>& proxy_ref) noexcept
                    : object_(nullptr) {
                // Step 1: spent-token check via core seam (object_storage
                // directly, NOT through decorator chain). Must happen before
                // touching control block or any decorator seam.
                if (!proxy_ref.core_valid()) {
                    contract_violation("using a spent exclusive_access");
                }
                // Step 2: exclusivity check.
                // Uses core_control() from root_slot, not the decorator chain.
#if LSE_ACCESS_CHECKING
                // Debug: all recipes check exclusivity. Bare recipes get a
                // control block in Debug, so orphan readers are caught here.
                {
                    auto* ctrl = proxy_ref.core_control();
                    if (ctrl && !ctrl->is_exclusive()) {
                        contract_violation(
                                "write expression while readers are active");
                    }
                }
#else
                // Release: only shared recipes have a control block to check.
                if (Shared) {
                    auto* ctrl = proxy_ref.core_control();
                    if (!ctrl || !ctrl->is_exclusive()) {
                        contract_violation(
                                "write expression while readers are active");
                    }
                }
#endif
                // Step 3: admission passed — NOW invoke the decorator write seam.
                // Side effects (audit counters, etc.) only fire on admitted writes.
                object_ = proxy_ref.mutable_write_object();
            }

            write_arrow(const write_arrow&) = delete;
            write_arrow& operator=(const write_arrow&) = delete;

            write_arrow(write_arrow&& rhs) noexcept
                    : object_(std::exchange(rhs.object_, nullptr)) {}

            write_arrow& operator=(write_arrow&&) = delete;

            T* operator->() const noexcept {
                LSE_UNLIKELY_IF (!object_) {
                    contract_violation("dereferencing a moved-from write_arrow");
                }
                return object_;
            }
            T& operator*() const noexcept {
                LSE_UNLIKELY_IF (!object_) {
                    contract_violation("dereferencing a moved-from write_arrow");
                }
                return *object_;
            }

        private:
            T* object_;
        };

// ============================================================
// Facades.
//
// Base order is deliberate:
//   lineage_root_slot, impl_type
// Bases are destroyed in reverse order, so the decorator implementation dies
// before root_slot releases its reference. root_slot's dtor calls
// lineage_control::release(), which self-deletes when refcount reaches 0.
// ============================================================
        template <typename T, typename DecoratorList, typename Policy>
        class proxy;

        template <typename T, typename DecoratorList>
        class proxy<T, DecoratorList, ro_tag>
                : private lineage_root_slot,
                  public materialize_t<T, ro_tag, DecoratorList> {
            using impl_type = materialize_t<T, ro_tag, DecoratorList>;
            static constexpr bool shared_recipe = contains<shared, DecoratorList>::value;

        public:
            using value_type = T;
            using policy_type = ro_tag;
            using decorator_list = DecoratorList;

            proxy(const proxy&) noexcept = default;
            proxy(proxy&&) noexcept = default;

            proxy& operator=(const proxy&) = delete;
            proxy& operator=(proxy&&) = delete;

            ~proxy() = default;

            const T* operator->() const noexcept {
                const T* object = this->const_object();
                LSE_UNLIKELY_IF (!object) {
                    contract_violation("dereferencing an empty shared_access");
                }
                return object;
            }

            template <typename U = T,
                    std::enable_if_t<std::is_same<U, T>::value &&
                            std::is_constructible<U, const U&>::value>* = nullptr>
            T clone() const
                noexcept(std::is_nothrow_constructible<U, const U&>::value) {
                const T* object = this->const_object();
                if (!object) {
                    contract_violation("cloning through an empty shared_access");
                }

                return T(*object);
            }

            explicit operator bool() const noexcept { return this->valid_object(); }

            template <typename F>
            auto read(F f) const
                noexcept(noexcept(std::declval<F&>()(std::declval<const T&>())))
                -> decltype(std::declval<F&>()(std::declval<const T&>())) {
                const T* object = this->const_object();
                if (!object) {
                    contract_violation("reading through an empty shared_access");
                }
                return f(*object);
            }

        private:
            // Root ro: inherits refcount=1 from lineage_control ctor.
            proxy(root_construct_tag,
                  T* object,
                  lineage_control* control) noexcept
                    : lineage_root_slot(control),
                      impl_type(object, control) {}

            // Derived ro (borrow_ro): acquires a new reference in the base
            // (lineage_construct_tag ctor). The materialized-type contract
            // (materialize_list static_assert) guarantees the full decorator
            // chain is nothrow constructible, so this is unconditionally safe.
            proxy(lineage_construct_tag, T* object, lineage_control* control) noexcept
                    : lineage_root_slot(lineage_construct_tag{}, control),
                      impl_type(object, control) {}

            template <typename, typename, typename>
            friend class proxy;

            template <typename U, bool L>
            friend class write_arrow;

            template <typename... Decorators, typename U>
            friend auto make_ro(U& object)
            -> proxy<U, dedup_t<type_list<Decorators...>>, ro_tag>;

            template <typename T2, typename DL2>
            friend auto enable_shared(
                proxy<T2, DL2, rw_tag>&)
                -> proxy<T2, dedup_t<typename shared_list<DL2>::type>, rw_tag>;
        };

        template <typename T, typename DecoratorList>
        class proxy<T, DecoratorList, rw_tag>
                : private lineage_root_slot,
                  public materialize_t<T, rw_tag, DecoratorList> {
            using impl_type = materialize_t<T, rw_tag, DecoratorList>;
            static constexpr bool shared_recipe = contains<shared, DecoratorList>::value;

            proxy& assign_value(T&& value)
                noexcept(std::is_nothrow_assignable<T&, T&&>::value) {
                write_arrow<T, shared_recipe> guard(*this);
                *guard = std::move(value);
                return *this;
            }

        public:
            using value_type = T;
            using policy_type = rw_tag;
            using decorator_list = DecoratorList;
            using ro_type = proxy<T, DecoratorList, ro_tag>;

            proxy(const proxy&) = delete;
            proxy& operator=(const proxy&) = delete;

            proxy(proxy&&) noexcept = default;
            proxy& operator=(proxy&&) = delete;

            ~proxy() = default;

            write_arrow<T, shared_recipe> operator->() noexcept {
                return write_arrow<T, shared_recipe>(*this);
            }

            template <typename F>
            auto write(F f)
                noexcept(noexcept(std::declval<F&>()(std::declval<T&>())))
                -> decltype(std::declval<F&>()(std::declval<T&>())) {
                write_arrow<T, shared_recipe> guard(*this);
                return f(*guard);
            }

            template <typename U = T, std::enable_if_t<std::is_assignable<U&, U&&>::value>* = nullptr>
            proxy& assign(T value)
                noexcept(std::is_nothrow_assignable<U&, U&&>::value) {
                return assign_value(std::move(value));
            }

            template <typename U = T, std::enable_if_t<std::is_assignable<U&, U&&>::value>* = nullptr>
            proxy& operator=(T value)
                noexcept(std::is_nothrow_assignable<U&, U&&>::value) {
                return assign_value(std::move(value));
            }

            template <typename U = T,
                    std::enable_if_t<std::is_same<U, T>::value &&
                                     std::is_constructible<U, const U&>::value>* = nullptr>
            T clone() const
            noexcept(std::is_nothrow_constructible<U, const U&>::value) {
                const T* object = this->const_object();
                if (!object) {
                    contract_violation("cloning through an empty exclusive_access");
                }

                return T(*object);
            }

            // Non-destructive read reborrow. rw authority survives; the derived
            // ro acquires a new reference via core_control() (root_slot's
            // authoritative pointer, not the decorator chain).
            //
            // Contract: must NOT be called from inside a write expression.
            // lease replaces raw references; write lambdas must not capture
            // the rw proxy by reference. Violating this is a contract breach.
            ro_type borrow_ro() const noexcept {
                T* object = this->mutable_object();
                if (!object) {
                    contract_violation("borrowing from an empty exclusive_access");
                }
                return ro_type(
                        lineage_construct_tag{}, object, this->core_control());
            }

            // Destructive downgrade: rw dies, ro is born.
            //
            // Construct ro first (acquire +1), then release rw's ref (-1, net
            // zero). The materialized-type contract (materialize_list static_assert)
            // guarantees the full chain is nothrow, so this is unconditional noexcept.
            ro_type downgrade() noexcept {
                T* object = this->mutable_object();
                if (!object) {
                    contract_violation("downgrading an empty exclusive_access");
                }

                lineage_control* control = this->core_control();

                if (!control) {
                    // Bare Release path: no control block, just transfer.
                    ro_type result(lineage_construct_tag{}, object, nullptr);
                    this->invalidate();
                    return result;
                }

                // Shared/Debug path: construct ro first (acquire +1), then
                // release rw's reference (-1, net zero).
                ro_type result(lineage_construct_tag{}, object, control);
                this->detach_control()->release();
                this->invalidate();
                return result;
            }

            explicit operator bool() const noexcept { return this->valid_object(); }

            // Core validity — bypasses all decorators, checks object_storage
            // directly. Used by write_arrow for spent-token check before any
            // decorator seam or control block access.
            bool core_valid() const noexcept {
                return this->object_storage<T>::valid_object();
            }

            // Core token-state operation: clear the proxy completely.
            // Bypasses user decorators (which sit above track in the chain)
            // but goes through track_impl to clear its observational control_
            // pointer. This ensures a spent token has:
            //   object == null, core control == null, observational control == null
            void invalidate() noexcept {
                this->track_impl<object_storage<T>, rw_tag>::invalidate();
            }

        private:
            proxy(root_construct_tag,
                  T* object,
                  lineage_control* control) noexcept
                    : lineage_root_slot(control),
                      impl_type(object, control) {}

            template <typename, typename, typename>
            friend class proxy;

            template <typename U, bool L>
            friend class write_arrow;

            template <typename T2, typename DL2>
            friend auto enable_shared(
                proxy<T2, DL2, rw_tag>&)
                -> proxy<T2, dedup_t<typename shared_list<DL2>::type>, rw_tag>;

            template <typename... Decorators, typename U>
            friend auto make_rw(U& object)
            -> proxy<U, dedup_t<type_list<Decorators...>>, rw_tag>;
        };

// User-facing aliases.
        template <typename T, typename... Decorators>
        using shared_access = proxy<T, dedup_t<type_list<Decorators...>>, ro_tag>;

        template <typename T, typename... Decorators>
        using exclusive_access = proxy<T, dedup_t<type_list<Decorators...>>, rw_tag>;

// ============================================================
// Factories.
// ============================================================
        template <typename... Decorators, typename T>
        auto make_ro(T& object)
            -> proxy<T, dedup_t<type_list<Decorators...>>, ro_tag> {
            static_assert(!std::is_array<T>::value && !std::is_function<T>::value,
                          "make_ro requires an object type, not an array or function");
            using D = dedup_t<type_list<Decorators...>>;

            constexpr bool free = !contains<shared, D>::value;
            if (free) {
#if LSE_ACCESS_CHECKING
                // Debug: bare also gets a control block for orphan-reader
                // detection. Release: bare is truly bare (no control block).
                auto* control = new lineage_control(std::addressof(object));
                return proxy<T, D, ro_tag>(
                        root_construct_tag{},
                        std::addressof(object),
                        control);
#else
                return proxy<T, D, ro_tag>(
                        root_construct_tag{},
                        std::addressof(object),
                        nullptr);
#endif
            }

            auto* control = new lineage_control(std::addressof(object));

            return proxy<T, D, ro_tag>(
                    root_construct_tag{},
                    std::addressof(object),
                    control);
        }

        template <typename... Decorators, typename T>
        auto make_rw(T& object)
            -> proxy<T, dedup_t<type_list<Decorators...>>, rw_tag> {
            static_assert(!std::is_const<T>::value,
                          "cannot create exclusive_access for a const object");
            static_assert(!std::is_array<T>::value && !std::is_function<T>::value,
                          "make_rw requires an object type, not an array or function");
            using D = dedup_t<type_list<Decorators...>>;

            constexpr bool free = !contains<shared, D>::value;
            if (free) {
#if LSE_ACCESS_CHECKING
                auto* control = new lineage_control(std::addressof(object));
                return proxy<T, D, rw_tag>(
                        root_construct_tag{},
                        std::addressof(object),
                        control);
#else
                return proxy<T, D, rw_tag>(
                        root_construct_tag{},
                        std::addressof(object),
                        nullptr);
#endif
            }

            auto* control = new lineage_control(std::addressof(object));

            return proxy<T, D, rw_tag>(
                    root_construct_tag{},
                    std::addressof(object),
                    control);
        }

// ============================================================
// enable_shared: bare exclusive_access -> shared exclusive_access.
//
// Destructive: the source proxy is invalidated.
// The target decorator chain is constructed fresh from (object, control).
// Decorator state is deliberately not moved across this recipe change; some
// decorator state has no equivalent meaning once shared mode is enabled.
//
// Strong exception guarantee via speculative-acquire pattern:
//   prepare → construct → commit
// All rollback is handled by RAII (lineage_root_slot destructor).
// No manual catch blocks — the root_slot IS the cleanup.
//
// Control-block strategy:
//   - Existing control (Debug bare): acquire a speculative ref (+1),
//     construct shared proxy adopting it. If construction throws, the
//     proxy's root_slot destructor releases the speculative ref (net 0).
//     rw's original ref is untouched. On success, detach rw's ref (-1).
//   - No control (Release bare): allocate new control (refcount=1),
//     construct. If construction throws, root_slot destructor releases
//     the initial ref → refcount 0 → self-delete. rw untouched.
// ============================================================
        template <typename T, typename DecoratorList>
        auto enable_shared(
            proxy<T, DecoratorList, rw_tag>& rw)
            -> proxy<T, dedup_t<typename shared_list<DecoratorList>::type>, rw_tag> {
            static_assert(!contains<shared, DecoratorList>::value,
                          "enable_shared requires a bare (non-shared) exclusive_access");
            using shared_proxy =
                proxy<T, dedup_t<typename shared_list<DecoratorList>::type>, rw_tag>;

            T* object = rw.mutable_object();
            if (!object) {
                contract_violation("sharing an empty exclusive_access");
            }

            lineage_control* control = rw.core_control();

            if (control) {
                // Existing control (Debug bare): check exclusivity before
                // proceeding. An orphan reader (borrow_ro still alive) means
                // the lineage is not exclusive — sharing it would be unsound.
                if (!control->is_exclusive()) {
                    contract_violation(
                            "enable_shared while readers are active");
                }
                // Acquire speculative ref, construct, then commit by
                // releasing rw's ref. RAII handles rollback: if construction
                // throws, the proxy's root_slot destructor releases the
                // speculative ref. rw is untouched (strong exception guarantee).
                control->acquire();
                shared_proxy result(root_construct_tag{}, object, control);
                rw.detach_control()->release();
                rw.invalidate();
                return result;
            }

            // Release bare: no control block. Allocate new one.
            // RAII handles rollback: if construction throws, root_slot
            // destructor releases the initial ref → self-delete. rw untouched.
            auto* new_control = new lineage_control(object);
            shared_proxy result(root_construct_tag{}, object, new_control);
            rw.invalidate();
            return result;
        }

} // namespace access
} // namespace lease

#endif // LEASE_FACADE_HPP
