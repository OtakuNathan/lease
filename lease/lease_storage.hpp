// lease_storage.hpp — Layer 1: raw storage, lineage control, mandatory track.
//
// Internal header: include via lease.hpp only. Macros (LSE_FORCE_INLINE,
// LSE_LIKELY_IF, ...) are defined by the umbrella header before this file is seen.
//
// This layer owns everything that must exist for a proxy to have a referent
// and a lifetime:
//   - contract_violation       : the single abort-on-contract-breach sink
//   - lineage_control          : intrusive ref-counted control block
//   - root_registry            : Debug-only single-lineage provenance checker
//   - lineage_root_slot        : RAII reference holder (acquire on copy, release on dtor)
//   - object_storage           : the terminal storage layer of every recipe
//   - track_impl / track       : mandatory layer anchoring the lineage control
//
// No decorator policy lives here; that is Layer 2 (lease_decorators.hpp).

#ifndef LEASE_STORAGE_HPP
#define LEASE_STORAGE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace lease {
namespace access {

        struct ro_tag {};
        struct rw_tag {};
        struct root_construct_tag {};
        struct lineage_construct_tag {};

        [[noreturn]] inline void contract_violation(const char* message) noexcept {
            std::fprintf(stderr, "lease::access contract violation: %s\n", message);
            std::abort();
        }

// ============================================================
// Debug-only root provenance checker.
//
// It tracks the referent object's address, not any memory reachable through T.
// Release assumes the root-lineage precondition and erases the global registry.
// ============================================================
        class root_registry {
        public:
#if LSE_ACCESS_CHECKING
            static void acquire(const void* object, const void* lineage) {
                if (!object || !lineage) {
                    contract_violation("invalid lineage registration");
                }

                auto& self = instance();
                std::lock_guard<std::mutex> lock(self.mutex_);

                const auto result = self.table_.emplace(object, lineage);
                if (!result.second && result.first->second != lineage) {
                    contract_violation(
                            "referent already belongs to another live proxy lineage");
                }
            }

            static void release(const void* object, const void* lineage) noexcept {
                auto& self = instance();
                std::lock_guard<std::mutex> lock(self.mutex_);

                const auto it = self.table_.find(object);
                if (it == self.table_.end() || it->second != lineage) {
                    contract_violation("unregistering unknown proxy lineage");
                }
                self.table_.erase(it);
            }

        private:
            // Deliberately immortal: the registry must outlive every proxy.
            // Static destruction runs in reverse order of construction
            // completion, so a static-storage owner constructed *before* the
            // first proxy (e.g. a global container filled during main) is
            // destroyed *after* this function-local static — and its handle
            // destructors would then call release() on a destroyed registry,
            // aborting at exit with "unregistering unknown proxy lineage".
            // Leaking one small table (Debug builds only) removes the ordering
            // dependency instead of relying on luck.
            static root_registry& instance() {
                static root_registry* registry = new root_registry();
                return *registry;
            }

            std::unordered_map<const void*, const void*> table_;
            std::mutex mutex_;
#else
            static void acquire(const void*, const void*) noexcept {}
            static void release(const void*, const void*) noexcept {}
#endif
        };

// ============================================================
// lineage_control — intrusive ref-counted control block.
//
// The control block is born with refcount = 1 (the creator's reference).
// Every proxy that holds a reference calls acquire() (+1) on copy and
// release() (-1) on destruction. When refcount reaches 0, the control
// block deletes itself and cleans up the Debug provenance entry.
//
// Write authority: refcount == 1 means only one participant exists —
// exclusive by construction. refcount > 1 means readers are active →
// contract violation (always checked — one atomic load, negligible cost).
//
// No writer bit, no root bit, no CAS loop, no backoff. The count IS the lock.
// ============================================================
        class lineage_control final : public pooling_base<lineage_control> {
        public:
            // Resource preparation, not topology mutation: may throw
            // (Debug root_registry::acquire does unordered_map::emplace).
            // This is fine — make_ro/make_rw/enable_shared are allowed to
            // fail; topology operations (move/borrow/downgrade/release) never
            // create a lineage_control and remain noexcept.
            explicit lineage_control(const void* object)
                    : object_(object) {
                root_registry::acquire(object_, this);
            }

            lineage_control(const lineage_control&) = delete;
            lineage_control& operator=(const lineage_control&) = delete;

            // Add a reference (e.g. ro copy, borrow_ro).
            void acquire() noexcept {
                refcount_.fetch_add(1, std::memory_order_acq_rel);
            }

            // Remove a reference. When refcount reaches 0, clean up Debug
            // provenance and self-delete.
            void release() noexcept {
                if (refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    root_registry::release(object_, this);
                    delete this;
                }
            }

            // Debug-only exclusive-write check: only one participant.
            bool is_exclusive() const noexcept {
                return refcount_.load(std::memory_order_acquire) == 1;
            }

            // Observational (for audit/debug decorators).
            std::uint64_t refcount_value() const noexcept {
                return refcount_.load(std::memory_order_relaxed);
            }

            const void* object_id() const noexcept { return object_; }

        private:
            ~lineage_control() noexcept = default;

            const void* object_;
            std::atomic<std::uint64_t> refcount_{1};
        };

// ============================================================
// lineage_root_slot — RAII reference holder.
//
// Every proxy (rw or ro) inherits this base. It holds a raw pointer to
// lineage_control and manages one reference:
//   - Default ctor: control_ = nullptr (bare proxy, no control block).
//   - Pointer ctor: inherits the initial refcount=1 from lineage_control's
//     ctor. Does NOT acquire — the creator's reference is already counted.
//   - Copy ctor: acquires a new reference (+1). This is how derived ro
//     proxies (borrow_ro, ro copy) get their own reference.
//   - Move ctor: transfers the pointer, source becomes nullptr. No acquire.
//     This is how downgrade works — rw's reference becomes ro's, refcount
//     unchanged.
//   - Destructor: releases the reference (-1). If refcount reaches 0,
//     lineage_control self-deletes.
//
// The slot is declared before the impl_type base so its destructor runs
// AFTER the decorator chain has been destroyed (reverse base order).
// ============================================================
        class lineage_root_slot {
        protected:
            lineage_root_slot() noexcept : control_(nullptr) {}

            // Root construction: inherits the refcount=1 from lineage_control.
            explicit lineage_root_slot(lineage_control* control) noexcept
                    : control_(control) {}

            // Derived construction: acquires a new reference in the base,
            // before impl_type is constructed. If impl_type ctor throws, the
            // base destructor releases the already-acquired reference.
            explicit lineage_root_slot(lineage_construct_tag, lineage_control* control) noexcept
                    : control_(control) {
                if (control_) control_->acquire();
            }

            // Copy: acquire a new reference (derived ro / ro copy).
            lineage_root_slot(const lineage_root_slot& rhs) noexcept
                    : control_(rhs.control_) {
                if (control_) control_->acquire();
            }

            // Move: transfer the reference, source is emptied.
            lineage_root_slot(lineage_root_slot&& rhs) noexcept
                    : control_(rhs.control_) {
                rhs.control_ = nullptr;
            }

            lineage_root_slot& operator=(const lineage_root_slot&) = delete;
            lineage_root_slot& operator=(lineage_root_slot&&) = delete;

            ~lineage_root_slot() noexcept {
                if (control_) control_->release();
            }

            // ---- Core authority accessors ----
            // These are the ONLY methods core operations (write_arrow,
            // borrow_ro, downgrade) use to reach the control block.
            // The decorator chain's control_pointer() is observational only
            // and must never be used for authority decisions.

            // Authoritative read-only access to the control block.
            lineage_control* core_control() const noexcept {
                return control_;
            }

            // Transfer ownership: returns the pointer and clears the slot.
            // The caller becomes responsible for the reference.
            lineage_control* detach_control() noexcept {
                auto* c = control_;
                control_ = nullptr;
                return c;
            }

        private:
            lineage_control* control_;
        };

// ============================================================
// Implicit terminal implementation.
// object_storage never appears in user recipes.
// ============================================================
        template <typename T>
        class object_storage {
        public:
            using value_type = T;

            object_storage(T* object, lineage_control*) noexcept
                    : object_(object) {
                if (!object_) {
                    contract_violation("object_storage received null referent");
                }
            }

            object_storage(const object_storage&) noexcept = default;

            object_storage(object_storage&& rhs) noexcept
                    : object_(std::exchange(rhs.object_, nullptr)) {}

            object_storage& operator=(const object_storage&) = delete;
            object_storage& operator=(object_storage&&) = delete;

        protected:
            // The referent-access seam every decorator layer builds on.
            T* mutable_object() const noexcept { return object_; }
            const T* const_object() const noexcept { return object_; }

            bool valid_object() const noexcept { return object_ != nullptr; }

            void invalidate() noexcept { object_ = nullptr; }

        private:
            T* object_;
        };

// ============================================================
// Mandatory default track decorator.
// It anchors the shared lineage control pointer; debug provenance lives
// in lineage_control.
//
// track is a decorator by shape (it has an apply seam) but it is mandatory and
// may never be listed in a user recipe; it always sits directly above
// object_storage. It provides the control() / control_pointer() accessors the
// facade and the optional decorators rely on.
// ============================================================
        template <typename Inner, typename Policy>
        class track_impl : public Inner {
        public:
            using value_type = typename Inner::value_type;
            using policy_type = Policy;

            track_impl(value_type* object, lineage_control* control) noexcept
                    : Inner(object, control), control_(control) {
            }

            track_impl(const track_impl&) noexcept = default;

            track_impl(track_impl&& rhs) noexcept
                    : Inner(std::move(rhs)),
                      control_(std::exchange(rhs.control_, nullptr)) {}

            track_impl& operator=(const track_impl&) = delete;
            track_impl& operator=(track_impl&&) = delete;

        protected:
            lineage_control& control() const noexcept {
                if (!control_) {
                    contract_violation("accessing an empty proxy lineage");
                }
                return *control_;
            }

            lineage_control* control_pointer() const noexcept { return control_; }

            value_type* mutable_write_object() const noexcept {
                return this->mutable_object();
            }

            void invalidate() noexcept {
                Inner::invalidate();
                control_ = nullptr;
            }

        private:
            lineage_control* control_;
        };

        struct track {
            template <typename Inner, typename Policy>
            using apply = track_impl<Inner, Policy>;
        };

// Internal terminal sentinel.
        struct object_leaf {};

} // namespace access
} // namespace lease

#endif // LEASE_STORAGE_HPP
