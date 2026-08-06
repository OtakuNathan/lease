// lease_facade.hpp — Layer 3: proxy facades, factories, writer exclusion.
//
// Internal header: include via lease.hpp only.
//
// This layer consumes the layers below and presents the public surface:
//   - materialize_*  : turns a recipe (type_list of decorators) into the final
//                      impl type
//   - writer_scope / write_arrow : per-expression writer exclusion
//   - proxy<T, DecoratorList, ro_tag|rw_tag> : the shared/exclusive facades
//   - shared_access / exclusive_access aliases
//   - make_ro / make_rw factories
//
// Destructive conversions (enable_locking(), downgrade()) take no &&: like
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
            // Instantiation gate: assemble the full recipe (user decorators +
            // mandatory track + object_leaf), then dedup. This is the single
            // convergence point — every recipe path (direct user, lock_list
            // prepend, future auto-inject) flows through here, so dedup at this
            // layer catches all duplicates regardless of origin.
            // First occurrence wins: user-declared decorators are authoritative
            // over any auto-injected ones.
            using raw = type_list<Nodes..., track, object_leaf>;
            using recipe = dedup_t<raw>;
            // Unpack the deduped type_list back into variadic args for
            // materialize_nodes.
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

            // The recipe is exactly what the user wrote: bare = single-threaded
            // (free), read_lock = multi-threaded (atomic). No implicit layers.
            // Dedup happens at the instantiation point (materialize_recipe),
            // not here — so every recipe path (direct, lock_list, future
            // auto-inject) converges through the same dedup gate.
            using type = typename materialize_recipe<
                    T, Policy, type_list<Decorators...>>::type;
        };

        template <typename T, typename Policy, typename List>
        using materialize_t = typename materialize_list<T, Policy, List>::type;

// ============================================================
// Writer exclusion is compiled in or out with the recipe. Locked: the
// original CAS protocol. Unlocked: an empty scope, so operator-> is free.
// ============================================================
        template <typename T, typename DecoratorList, typename Policy>
        class proxy;

        template <bool Locked>
        class writer_scope;

        template <>
        class writer_scope<true> {
        public:
            explicit writer_scope(lineage_control* control) noexcept
                    : control_(control) {
                if (!control_) {
                    contract_violation("writer scope requires a live lineage");
                }
                access_counter::acquire_writer(control_->state());
            }

            writer_scope(const writer_scope&) = delete;
            writer_scope& operator=(const writer_scope&) = delete;

            writer_scope(writer_scope&& rhs) noexcept
                    : control_(std::exchange(rhs.control_, nullptr)) {}

            writer_scope& operator=(writer_scope&&) = delete;

            ~writer_scope() noexcept {
                if (control_) {
                    access_counter::release_writer(control_->state());
                }
            }

        private:
            lineage_control* control_;
        };

        template <>
        class writer_scope<false> {
        public:
            // Unlocked mode: the referent pointer is intentionally ignored. The
            // scope is an empty guard that costs nothing and excludes nothing.
            explicit writer_scope(lineage_control*) noexcept {}
            writer_scope(const writer_scope&) = delete;
            writer_scope& operator=(const writer_scope&) = delete;
            writer_scope(writer_scope&&) noexcept = default;
            writer_scope& operator=(writer_scope&&) = delete;
            ~writer_scope() noexcept = default;
        };

        template <typename T, bool Locked>
        class write_arrow {
        public:
            // Constructed with the owning proxy *after* writer exclusion has
            // been acquired (member declaration order: scope_ before object_).
            // The referent pointer is resolved inside the lock, so decorator
            // write-side state (e.g. audit counters) mutates inside the
            // protocol's happens-before chain — never before acquire_writer.
            template <typename DecoratorList>
            write_arrow(proxy<T, DecoratorList, rw_tag>& proxy_ref) noexcept
                    : scope_(proxy_ref.control_pointer()), object_(nullptr) {
                object_ = proxy_ref.mutable_write_object();
                LSE_UNLIKELY_IF (!object_) {
                    contract_violation("dereferencing an empty exclusive_access");
                }
            }

            write_arrow(const write_arrow&) = delete;
            write_arrow& operator=(const write_arrow&) = delete;
            write_arrow(write_arrow&&) noexcept = default;
            write_arrow& operator=(write_arrow&&) = delete;

            T* operator->() const noexcept { return object_; }

        private:
            writer_scope<Locked> scope_;  // declared first: acquired first
            T* object_;
        };

// ============================================================
// Facades.
//
// Base order is deliberate:
//   lineage_root_slot, impl_type
// Bases are destroyed in reverse order, so the decorator implementation dies
// before root ownership can delete lineage_control. The reader share is held
// by the read_lock decorator layer (when the recipe is locked), which is part
// of impl_type and therefore releases its reader share before the root base
// is torn down. A bare recipe has no reader counting at all.
// ============================================================
        template <typename T, typename DecoratorList, typename Policy>
        class proxy;

        template <typename T, typename DecoratorList>
        class proxy<T, DecoratorList, ro_tag>
                : private lineage_root_slot,
                  public materialize_t<T, ro_tag, DecoratorList> {
            using impl_type = materialize_t<T, ro_tag, DecoratorList>;
            static constexpr bool locked = contains<read_lock, DecoratorList>::value;

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
                // No writer exclusion here: this reader itself blocks writers
                // (reader-preference protocol), so the snapshot it reads is
                // consistent by construction. Taking a writer scope from a
                // reader would self-deadlock — the reader count can never
                // reach zero while this proxy is alive.
                const T* object = this->const_object();
                if (!object) {
                    contract_violation("cloning through an empty shared_access");
                }

                return T(*object);
            }

            explicit operator bool() const noexcept { return this->valid_object(); }

        private:
            // Root ro: owns the unique control block and also contributes one reader.
            // A bare root passes a null owner/control: no lineage exists yet.
            proxy(root_construct_tag,
                  T* object,
                  lineage_control* control,
                  std::unique_ptr<lineage_control> owner) noexcept
                    : lineage_root_slot(std::move(owner)),
                      impl_type(object, control) {}

            // Derived ro: keeps only the stable raw pointer; reader count owns lifetime.
            proxy(lineage_construct_tag, T* object, lineage_control* control) noexcept
                : lineage_root_slot(), impl_type(object, control) {}

            template <typename, typename, typename>
            friend class proxy;

            template <typename... Decorators, typename U>
            friend auto make_ro(U& object)
            -> proxy<U, type_list<Decorators...>, ro_tag>;
        };

        template <typename T, typename DecoratorList>
        class proxy<T, DecoratorList, rw_tag>
                : private lineage_root_slot,
                  public materialize_t<T, rw_tag, DecoratorList> {
            using impl_type = materialize_t<T, rw_tag, DecoratorList>;
            static constexpr bool locked = contains<read_lock, DecoratorList>::value;

            proxy& assign_value(T&& value)
                noexcept(std::is_nothrow_assignable<T&, T&&>::value) {
                writer_scope<locked> scope(this->control_pointer());

                // Write expression: resolved inside the lock so decorator
                // write-side state mutates within the happens-before chain.
                T* object = this->mutable_write_object();
                if (!object) {
                    contract_violation(
                            "assigning through an empty exclusive_access"
                    );
                }

                *object = std::move(value);
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

            write_arrow<T, locked> operator->() noexcept {
                // The write_arrow constructor acquires writer exclusion first
                // and resolves the referent inside the lock (see write_arrow).
                return write_arrow<T, locked>(*this);
            }

            // Direct referent replacement. Value semantics only; no const T& API.
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
                writer_scope<locked> scope(this->control_pointer());

                const T* object = this->const_object();
                if (!object) {
                    contract_violation("cloning through an empty exclusive_access");
                }

                return T(*object);
            }

            // Non-destructive read reborrow. rw authority survives but its write
            // expressions wait until every derived shared_access is destroyed.
            ro_type borrow_ro() const noexcept {
                T* object = this->mutable_object();
                if (!object) {
                    contract_violation("borrowing from an empty exclusive_access");
                }
                return ro_type(
                        lineage_construct_tag{}, object, this->control_pointer());
            }

            // Destructive downgrade: move root ownership into the returned
            // shared_access. No && needed — like unique_ptr::release(), the
            // source handle is invalidated here and any later use of it is a
            // contract violation, never silent UB.
            // Bare roots (no lineage yet) transfer the bare referent instead;
            // the result is a bare shared_access with no control block.
            ro_type downgrade() noexcept {
                T* object = this->mutable_object();
                lineage_control* control = this->control_pointer();
                if (!object) {
                    contract_violation("downgrading an empty exclusive_access");
                }

                if (!control) {
                    ro_type result(lineage_construct_tag{}, object, nullptr);
                    this->invalidate();
                    return result;
                }

                auto owner = this->take_root_ownership();
                ro_type result(
                        root_construct_tag{}, object, control, std::move(owner));
                this->invalidate();
                return result;
            }

            explicit operator bool() const noexcept { return this->valid_object(); }

        private:
            proxy(root_construct_tag,
                  T* object,
                  lineage_control* control,
                  std::unique_ptr<lineage_control> owner) noexcept
                    : lineage_root_slot(std::move(owner)),
                      impl_type(object, control) {}

            template <typename, typename, typename>
            friend class proxy;

            template <typename U, bool L>
            friend class write_arrow;

            // Grant enable_locking access to private constructors across
            // all proxy instantiations (source bare + target locked).
            template <typename T2, typename... Ds2>
            friend auto enable_locking(
                proxy<T2, type_list<Ds2...>, rw_tag>&) noexcept
                -> proxy<T2, typename lock_list<type_list<Ds2...>>::type, rw_tag>;

            template <typename... Decorators, typename U>
            friend auto make_rw(U& object)
            -> proxy<U, type_list<Decorators...>, rw_tag>;
        };

// User-facing aliases are mostly useful for contracts/tests; factory return
// types are intentionally best consumed with auto.
        template <typename T, typename... Decorators>
        using shared_access = proxy<T, type_list<Decorators...>, ro_tag>;

        template <typename T, typename... Decorators>
        using exclusive_access = proxy<T, type_list<Decorators...>, rw_tag>;

// ============================================================
// Factories: users only list the extra dolls they want to wrap.
// Facade, track, and object_storage are implicit.
// Exactly one root proxy starts with unique_ptr ownership.
// ============================================================
        template <typename... Decorators, typename T>
        auto make_ro(T& object)
        -> proxy<T, type_list<Decorators...>, ro_tag> {
            static_assert(!std::is_array<T>::value && !std::is_function<T>::value,
                          "make_ro requires an object type, not an array or function");

            // A bare root (no read_lock) defers the shared era entirely: no
            // heap control block, no atomics, no registry entry. The lineage is
            // created lazily if/when an exclusive root is passed to
            // enable_locking() to enter the locked (shared) era.
            constexpr bool free =
                    !contains<read_lock, type_list<Decorators...>>::value;
            if (free) {
                return proxy<T, type_list<Decorators...>, ro_tag>(
                        root_construct_tag{},
                        std::addressof(object),
                        nullptr,
                        nullptr);
            }

            auto owner = std::make_unique<lineage_control>(std::addressof(object));
            lineage_control* control = owner.get();

            return proxy<T, type_list<Decorators...>, ro_tag>(
                    root_construct_tag{},
                    std::addressof(object),
                    control,
                    std::move(owner));
        }

        template <typename... Decorators, typename T>
        auto make_rw(T& object)
        -> proxy<T, type_list<Decorators...>, rw_tag> {
            static_assert(!std::is_const<T>::value,
                          "cannot create exclusive_access for a const object");
            static_assert(!std::is_array<T>::value && !std::is_function<T>::value,
                          "make_rw requires an object type, not an array or function");

            // Same lazy-lineage rule as make_ro: bare roots are free.
            constexpr bool free =
                    !contains<read_lock, type_list<Decorators...>>::value;
            if (free) {
                return proxy<T, type_list<Decorators...>, rw_tag>(
                        root_construct_tag{},
                        std::addressof(object),
                        nullptr,
                        nullptr);
            }

            auto owner = std::make_unique<lineage_control>(std::addressof(object));
            lineage_control* control = owner.get();

            return proxy<T, type_list<Decorators...>, rw_tag>(
                    root_construct_tag{},
                    std::addressof(object),
                    control,
                    std::move(owner));
        }

// ============================================================
// enable_locking: bare exclusive_access -> locked exclusive_access.
//
// Destructive: the source proxy hands its root ownership to the returned
// locked proxy and is invalidated. No && needed — like unique_ptr::release(),
// the source handle is dead after the call and any later use trips a
// contract violation.
//
// Why static_assert, not SFINAE: this free function needs a friend declaration
// to reach the private root-construct constructor. Empirically (tested, g++):
//   - enable_if as a defaulted template param on the function  -> AMBIGUOUS:
//     the friend decl (no defaulted param) and the definition become two
//     distinct templates, both candidates;
//   - mirroring the enable_if onto the friend decl              -> ILLEGAL:
//     "default template arguments may not be used in template friend
//     declarations" (hard language rule);
//   - enable_if in the trailing return type                     -> AMBIGUOUS:
//     same split — friend names proxy<...> return, defn names enable_if_t<...>.
// SFINAE alters the function template's signature; friend matching requires
// identical signatures; the two are in direct conflict for one function. The
// member lock() could SFINAE (members befriend themselves), but a .lock()
// method on a *bare* (lock-free) proxy is misleading, so this is a free
// function + static_assert instead. The static_assert also gives a clearer
// message than "no matching function" would.
//
// This is the lazy-lineage seam: the shared era (heap control block,
// atomics, registry entry) begins exactly here. lineage_control's constructor
// registers with the root_registry, so a second bare root locking the same
// referent aborts in Debug. Release builds skip that check by contract.
//
// Orphan-reader contract: any shared_access derived from the source proxy
// before enable_locking (via borrow_ro) must be destroyed first.
// ============================================================
        template <typename T, typename... Decorators>
        auto enable_locking(
            proxy<T, type_list<Decorators...>, rw_tag>& rw) noexcept
            -> proxy<T, typename lock_list<type_list<Decorators...>>::type, rw_tag> {
            static_assert(!contains<read_lock, type_list<Decorators...>>::value,
                          "enable_locking requires a bare (unlocked) exclusive_access");
            using locked_proxy =
                proxy<T, typename lock_list<type_list<Decorators...>>::type, rw_tag>;

            T* object = rw.mutable_object();
            if (!object) {
                contract_violation("locking an empty exclusive_access");
            }

            auto owner = std::make_unique<lineage_control>(object);
            lineage_control* control = owner.get();
            locked_proxy result(
                    root_construct_tag{}, object, control, std::move(owner));
            rw.invalidate();
            return result;
        }

} // namespace access
} // namespace lease

#endif // LEASE_FACADE_HPP
