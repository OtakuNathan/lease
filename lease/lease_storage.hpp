// lease_storage.hpp — Layer 1: raw storage, lineage control, mandatory track.
//
// Internal header: include via lease.hpp only. Macros (LSE_FORCE_INLINE,
// LSE_LIKELY_IF, ...) are defined by the umbrella header before this file is seen.
//
// This layer owns everything that must exist for a proxy to have a referent
// and a lifetime:
//   - contract_violation       : the single abort-on-contract-breach sink
//   - access_state / counter   : the atomic reader/writer/root admission word
//   - root_registry            : Debug-only single-lineage provenance checker
//   - lineage_control          : heap control block (created eagerly by locked
//                                roots, lazily by lock())
//   - lineage_root_slot        : unique_ptr root ownership base
//   - object_storage           : the terminal storage layer of every recipe
//   - track_impl / track       : mandatory layer anchoring the lineage control
//
// No decorator policy lives here; that is Layer 2 (lease_decorators.hpp).

#ifndef LEASE_STORAGE_HPP
#define LEASE_STORAGE_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
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
// Mandatory lineage-local reader-preferred read/write admission +
// intrusive lineage lifetime.
//
// word layout:
//   bit0      : writer operation active
//   bit1      : root proxy still owns the control block
//   bits2..63 : live shared_access count * 4
//
// The root proxy starts with unique_ptr ownership. If it dies while readers
// remain, it clears root_alive and releases the unique_ptr. The last reader
// observes root_alive == false and deletes the control block: the familiar
// "last closer turns off the lights" protocol.
// ============================================================
        struct access_state {
            static constexpr std::uint64_t writer_bit = 1;
            static constexpr std::uint64_t root_bit = 2;
            static constexpr std::uint64_t reader_unit = 4;

            std::atomic<std::uint64_t> word{root_bit};
        };

// ============================================================
// Adaptive spin backoff, adapted from flux_foundry/utility/back_off.h
// (backoff_strategy), so this TU keeps the same contention behavior
// as the parent library.
//
// Two-phase strategy: the first `spin_limit` calls busy-wait with
// exponentially increasing pause bursts (count 1,2,4,... capped at
// max_loop), then fall back to std::this_thread::yield(). pause is a
// few dozen cycles and cache-friendly; yield is ~1us and reschedules.
// ============================================================
        template <size_t spin_limit = 16, size_t max_loop = 1024>
        struct backoff_strategy {
            size_t count {1};
            size_t steps {0};

            void reset() noexcept {
                count = 1;
                steps = 0;
            }

            void yield() noexcept {
                if (steps < spin_limit) {
                    for (size_t i = 0; i < count; ++i) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
                        _mm_pause();
#elif defined(__aarch64__)
                        __asm__ __volatile__("yield");
#elif defined(_M_ARM64)
                        __yield();
#else
                        std::atomic_signal_fence(std::memory_order_relaxed);
#endif
                    }
                    count = std::min(count << 1, max_loop);
                    ++steps;
                } else {
                    std::this_thread::yield();
                }
            }
        };

        namespace access_counter {

            constexpr std::uint64_t writer_bit = access_state::writer_bit;
            constexpr std::uint64_t root_bit = access_state::root_bit;
            constexpr std::uint64_t reader_unit = access_state::reader_unit;
            constexpr std::uint64_t reader_mask = ~(writer_bit | root_bit);
            constexpr std::uint64_t max_readers = (UINT64_MAX >> 2);

            inline std::uint64_t reader_count(std::uint64_t word) noexcept {
                return (word & reader_mask) >> 2;
            }

            LSE_FORCE_INLINE void acquire_reader(access_state& state) noexcept {
                std::uint64_t current = state.word.load(std::memory_order_relaxed);
                backoff_strategy<> backoff;

                for (;;) {
                    if ((current & writer_bit) != 0) {
                        current = state.word.load(std::memory_order_acquire);
                        backoff.yield();
                        continue;
                    }

                    if (reader_count(current) == max_readers) {
                        contract_violation("reader count overflow");
                    }

                    if (state.word.compare_exchange_weak(
                            current,
                            current + reader_unit,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        return;
                    }
                }
            }

// Returns true when this reader is the last live participant after the root
// has already gone. The caller then owns final deletion of lineage_control.
            LSE_FORCE_INLINE bool release_reader(access_state& state) noexcept {
                const std::uint64_t previous =
                        state.word.fetch_sub(reader_unit, std::memory_order_acq_rel);

                if (reader_count(previous) == 0 || (previous & writer_bit) != 0) {
                    contract_violation("bad reader lifecycle");
                }

                return reader_count(previous) == 1 && (previous & root_bit) == 0;
            }

            LSE_FORCE_INLINE void acquire_writer(access_state& state) noexcept {
                std::uint64_t expected = root_bit;
                backoff_strategy<> backoff;

                for (;;) {
                    if (state.word.compare_exchange_weak(
                            expected,
                            root_bit | writer_bit,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        return;
                    }

                    if ((expected & root_bit) == 0) {
                        contract_violation("writer used after root ownership was released");
                    }

                    // Reader preference: new readers may still enter while this writer waits.
                    expected = root_bit;
                    backoff.yield();
                }
            }

            LSE_FORCE_INLINE void release_writer(access_state& state) noexcept {
                std::uint64_t expected = root_bit | writer_bit;

                if (!state.word.compare_exchange_strong(
                        expected,
                        root_bit,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    contract_violation("bad writer lifecycle");
                }
            }

// Releases root ownership. Returns true if no readers remain and the root's
// unique_ptr should delete the control block immediately. Otherwise the root
// releases its unique_ptr and the last reader will delete the orphan.
            LSE_FORCE_INLINE bool release_root(access_state& state) noexcept {
                const std::uint64_t previous =
                        state.word.fetch_and(~root_bit, std::memory_order_acq_rel);

                if ((previous & root_bit) == 0) {
                    contract_violation("root ownership released twice");
                }
                if ((previous & writer_bit) != 0) {
                    contract_violation("root destroyed during an active write expression");
                }

                return reader_count(previous) == 0;
            }

        } // namespace access_counter

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
            static root_registry& instance() {
                static root_registry registry;
                return registry;
            }

            std::unordered_map<const void*, const void*> table_;
            std::mutex mutex_;
#else
            static void acquire(const void*, const void*) noexcept {}
            static void release(const void*, const void*) noexcept {}
#endif
        };

// Shared by every proxy in one legal lineage. Ownership begins in exactly
// one root proxy. Derived readers keep only a stable raw pointer.
        class lineage_control {
        public:
            explicit lineage_control(const void* object) noexcept
                    : object_(object) {
                root_registry::acquire(object_, this);
            }

            ~lineage_control() noexcept {
                if (state_.word.load(std::memory_order_relaxed) != 0) {
                    contract_violation("lineage destroyed with live participants");
                }
                root_registry::release(object_, this);
            }

            lineage_control(const lineage_control&) = delete;
            lineage_control& operator=(const lineage_control&) = delete;

            access_state& state() noexcept { return state_; }
            const void* object_id() const noexcept { return object_; }

        private:
            const void* object_;
            access_state state_;
        };

// Root ownership is unique, address-stable, and deliberately not copied.
// It is a base declared before the implementation base so its destructor runs
// after the decorator chain has already been destroyed.
        class lineage_root_slot {
        protected:
            lineage_root_slot() noexcept = default;

            // A root may carry no ownership: bare (single-thread) roots
            // defer lineage creation until lock() and hold a bare referent
            // instead. Locked roots always pass a non-null owner (the factories
            // guarantee it), so the non-null check lives there.
            explicit lineage_root_slot(
                    std::unique_ptr<lineage_control> owner) noexcept
                    : owner_(std::move(owner)) {}

            // Copying a shared_access creates a derived reader, not a second root owner.
            lineage_root_slot(const lineage_root_slot&) noexcept {}

            lineage_root_slot(lineage_root_slot&&) noexcept = default;

            lineage_root_slot& operator=(const lineage_root_slot&) = delete;
            lineage_root_slot& operator=(lineage_root_slot&&) = delete;

            ~lineage_root_slot() noexcept {
                release_root_ownership();
            }

            std::unique_ptr<lineage_control> take_root_ownership() noexcept {
                if (!owner_) {
                    contract_violation("proxy does not own the lineage root");
                }
                return std::move(owner_);
            }

        private:
            void release_root_ownership() noexcept {
                if (!owner_) {
                    return;
                }

                if (access_counter::release_root(owner_->state())) {
                    owner_.reset();
                } else {
                    // Readers keep the raw stable pointer. The final reader deletes it.
                    (void)owner_.release();
                }
            }

            std::unique_ptr<lineage_control> owner_;
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
// It anchors the shared lineage control; debug provenance lives in that control.
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
                // control_ may be null for bare (single-thread) lineages
                // whose shared control has not been created yet. control() is
                // therefore locked-only; control_pointer() is the null-safe view.
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

            // Write-expression seam (optional). Write expressions (operator->
            // through write_arrow) call this *after* writer exclusion has been
            // acquired, so decorators that override it observe/update state
            // inside the protocol's happens-before chain. Management operations
            // (borrow_ro / downgrade / lock) use mutable_object directly and
            // are not write expressions.
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
