// lease_pool.hpp — Fixed-size object pool for lineage_control.
//
// Internal header: include via lease.hpp only.
//
// Design borrowed from flux_foundry's static_mem_pool + pooling_base:
//   - Lock-free Treiber stack with seq+offset packing (ABA-safe)
//   - Pre-allocated slab (no malloc in the hot path)
//   - O(1) pointer arithmetic for ownership check
//   - Thread-local cache for contention-free fast path
//   - CRTP mixin: inherit pooling_base<T> → get pooled new/delete
//
// Specialized for fixed-size blocks (lineage_control is ~16 bytes).
// No size-class matching, no epoch system — one slab, one size.
//
// Three-tier allocation strategy:
//   1. Thread-local cache (array pop, ~0 ns, no atomics)
//   2. Global lock-free stack (CAS, ~few ns)
//   3. malloc fallback (system call, ~50-100 ns)

#ifndef LEASE_POOL_HPP
#define LEASE_POOL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace lease {
namespace access {

// ============================================================
// Lock-free Treiber stack with sequence-number ABA protection.
//
// Each slot is identified by a packed (seq, offset) word:
//   - offset: index into the pre-allocated node array [0, capacity)
//   - seq: monotonic counter, incremented on each push
//
// The head word packs (seq, offset). CAS on head succeeds only if
// both seq and offset match — preventing ABA where a node is popped,
// modified, and pushed back between two reads of the same head value.
//
// Capacity must be a power of two. The offset field uses log2(capacity)+1
// bits; the remaining bits are the sequence counter.
// ============================================================
    template <typename T, std::size_t Capacity>
    class lf_stack {
        static_assert(Capacity > 0, "Capacity must be > 0");
        static_assert((Capacity & (Capacity - 1)) == 0,
                      "Capacity must be a power of two");
        static_assert(std::is_nothrow_move_constructible<T>::value,
                      "T must be nothrow move constructible");

    private:
        // Number of bits needed to encode [0, Capacity].
        static constexpr std::size_t off_bits() noexcept {
            std::size_t i = 0;
            for (auto cap = Capacity; cap >= 1;) { cap >>= 1; ++i; }
            return i;
        }

        static constexpr std::uint64_t off_mask() noexcept {
            return (std::uint64_t{1} << off_bits()) - 1;
        }

        static constexpr std::uint64_t empty_tag() noexcept {
            return Capacity;  // offset == Capacity means "empty"
        }

        static constexpr std::uint64_t make_tag(std::uint64_t seq, std::uint64_t offset) noexcept {
            return (seq << off_bits()) | offset;
        }

        static constexpr std::uint64_t get_seq(std::uint64_t tag) noexcept {
            return tag >> off_bits();
        }

        static constexpr std::uint64_t get_offset(std::uint64_t tag) noexcept {
            return tag & off_mask();
        }

        struct node {
            T value;
            std::atomic<std::uint64_t> next;
        };

        // Cache-line padded to prevent false sharing between head and free list.
        alignas(64) std::atomic<std::uint64_t> head_;
        alignas(64) std::atomic<std::uint64_t> free_;
        node nodes_[Capacity];

        std::uint64_t pop_from_list(std::atomic<std::uint64_t>& list) noexcept {
            std::uint64_t h = list.load(std::memory_order_acquire);
            for (;;) {
                if (get_offset(h) == empty_tag()) {
                    return empty_tag();
                }
                std::uint64_t offset = get_offset(h);
                std::uint64_t next = nodes_[offset].next.load(std::memory_order_relaxed);
                if (list.compare_exchange_weak(h, next,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
            }
            return h;  // contains the seq+offset we popped
        }

        void push_to_list(std::atomic<std::uint64_t>& list, std::uint64_t tag) noexcept {
            std::uint64_t h = list.load(std::memory_order_acquire);
            for (;;) {
                nodes_[get_offset(tag)].next.store(h, std::memory_order_relaxed);
                if (list.compare_exchange_weak(h, tag,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
            }
        }

    public:
        lf_stack() noexcept {
            head_.store(empty_tag(), std::memory_order_relaxed);
            // Build the free list: 0 → 1 → 2 → ... → Capacity (empty).
            for (std::size_t i = 0; i < Capacity; ++i) {
                nodes_[i].next.store(i + 1, std::memory_order_relaxed);
            }
            free_.store(make_tag(0, 0), std::memory_order_relaxed);
        }

        lf_stack(const lf_stack&) = delete;
        lf_stack& operator=(const lf_stack&) = delete;
        lf_stack(lf_stack&&) = delete;
        lf_stack& operator=(lf_stack&&) = delete;

        bool push(T&& val) noexcept {
            std::uint64_t tag = pop_from_list(free_);
            if (get_offset(tag) == empty_tag()) {
                return false;  // pool exhausted
            }
            std::uint64_t seq = get_seq(tag);
            std::uint64_t offset = get_offset(tag);
            nodes_[offset].value = std::move(val);
            push_to_list(head_, make_tag(seq + 1, offset));
            return true;
        }

        bool push(const T& val) noexcept {
            std::uint64_t tag = pop_from_list(free_);
            if (get_offset(tag) == empty_tag()) {
                return false;
            }
            std::uint64_t seq = get_seq(tag);
            std::uint64_t offset = get_offset(tag);
            nodes_[offset].value = val;
            push_to_list(head_, make_tag(seq + 1, offset));
            return true;
        }

        // Returns true if value was set, false if stack is empty.
        bool pop(T& out) noexcept {
            std::uint64_t tag = pop_from_list(head_);
            if (get_offset(tag) == empty_tag()) {
                return false;
            }
            std::uint64_t seq = get_seq(tag);
            std::uint64_t offset = get_offset(tag);
            out = std::move(nodes_[offset].value);
            push_to_list(free_, make_tag(seq, offset));
            return true;
        }
    };

// ============================================================
// Fixed-size slab pool.
//
// A single contiguous buffer carved into Capacity blocks of BlockSize.
// O(1) pointer arithmetic determines whether a pointer belongs to this
// slab and which block index it occupies.
// ============================================================
    template <std::size_t BlockSize, std::size_t Capacity = 64>
    class fixed_slab {
        static_assert((Capacity & (Capacity - 1)) == 0,
                      "Capacity must be a power of two");
        static_assert((BlockSize % alignof(std::max_align_t)) == 0,
                      "BlockSize must be a multiple of max_align_t");

        alignas(std::max_align_t) unsigned char storage_[Capacity * BlockSize];
        lf_stack<void*, Capacity> free_list_;

    public:
        fixed_slab() noexcept {
            // Populate the free list with all block addresses.
            for (std::size_t i = 0; i < Capacity; ++i) {
                void* p = storage_ + i * BlockSize;
                free_list_.push(p);
            }
        }

        fixed_slab(const fixed_slab&) = delete;
        fixed_slab& operator=(const fixed_slab&) = delete;

        void* allocate() noexcept {
            void* p = nullptr;
            if (free_list_.pop(p)) {
                return p;
            }
            return nullptr;  // slab exhausted
        }

        void deallocate(void* p) noexcept {
            if (!p) return;
            free_list_.push(p);
        }

        bool owns(const void* p) const noexcept {
            const auto* base = storage_;
            const auto* cur = static_cast<const unsigned char*>(p);
            return cur >= base && cur < base + sizeof(storage_);
        }
    };

// ============================================================
// Three-tier allocator: thread-local cache → global slab → malloc.
//
// The thread-local cache is a simple array-based freelist. Each thread
// gets its own cache, so the fast path has zero contention. When the
// cache is empty, it falls through to the global lock-free slab. When
// the slab is exhausted, it falls through to malloc.
// ============================================================
    template <std::size_t BlockSize, std::size_t Capacity = 64,
              std::size_t CacheCap = 128>
    class pooled_allocator {
        static_assert((CacheCap & (CacheCap - 1)) == 0,
                      "CacheCap must be a power of two");

        struct cache_t {
            void* slots[CacheCap];
            std::size_t top = 0;

            bool push(void* p) noexcept {
                if (top < CacheCap) {
                    slots[top++] = p;
                    return true;
                }
                return false;
            }

            void* pop() noexcept {
                if (top > 0) {
                    return slots[--top];
                }
                return nullptr;
            }

            ~cache_t() noexcept {
                // Return cached blocks to the global slab on thread exit.
                auto& slab = global_slab();
                while (top > 0) {
                    void* p = slots[--top];
                    if (slab.owns(p)) {
                        slab.deallocate(p);
                    } else {
                        std::free(p);
                    }
                }
            }
        };

        // Thread-local cache: zero-contention fast path.
        static cache_t& thread_cache() noexcept {
            static thread_local cache_t cache;
            return cache;
        }

        // Global slab: lock-free fallback.
        static fixed_slab<BlockSize, Capacity>& global_slab() noexcept {
            static fixed_slab<BlockSize, Capacity> slab;
            return slab;
        }

    public:
        static void* alloc() noexcept {
            // Tier 1: thread-local cache (~0 ns, no atomics).
            if (void* p = thread_cache().pop()) {
                return p;
            }
            // Tier 2: global lock-free slab (~few ns, CAS).
            if (void* p = global_slab().allocate()) {
                return p;
            }
            // Tier 3: malloc fallback (~50-100 ns).
            return std::malloc(BlockSize);
        }

        static void dealloc(void* p) noexcept {
            if (!p) return;
            // Fast path: return to thread-local cache.
            if (thread_cache().push(p)) {
                return;
            }
            // Cache full: return to global slab or free.
            auto& slab = global_slab();
            if (slab.owns(p)) {
                slab.deallocate(p);
            } else {
                std::free(p);
            }
        }
    };

// ============================================================
// CRTP mixin: inherit to get pooled operator new/delete.
//
// Usage:
//   class lineage_control : public pooling_base<lineage_control> { ... };
//
// The derived type must be final (prevents slicing in the pool).
// operator new/delete are routed through pooled_allocator, which uses
// the three-tier strategy (TLS cache → lock-free slab → malloc).
// ============================================================
    template <typename T, std::size_t CacheCap = 128>
    class pooling_base {
        // block_size and allocator_type are deferred to function scope:
        // T is incomplete at class definition time (CRTP).

    public:
        static void* operator new(std::size_t n) {
            if (n != sizeof(T)) {
                return ::operator new(n);
            }
            constexpr std::size_t align = alignof(std::max_align_t);
            constexpr std::size_t raw = sizeof(T);
            constexpr std::size_t bsz = (raw + align - 1) & ~(align - 1);
            void* p = pooled_allocator<bsz, 64, CacheCap>::alloc();
            if (!p) {
                throw std::bad_alloc();
            }
            return p;
        }

        static void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
            if (n != sizeof(T)) {
                return ::operator new(n, std::nothrow);
            }
            constexpr std::size_t align = alignof(std::max_align_t);
            constexpr std::size_t raw = sizeof(T);
            constexpr std::size_t bsz = (raw + align - 1) & ~(align - 1);
            return pooled_allocator<bsz, 64, CacheCap>::alloc();
        }

        static void operator delete(void* p) noexcept {
            if (!p) return;
            constexpr std::size_t align = alignof(std::max_align_t);
            constexpr std::size_t raw = sizeof(T);
            constexpr std::size_t bsz = (raw + align - 1) & ~(align - 1);
            pooled_allocator<bsz, 64, CacheCap>::dealloc(p);
        }

        static void operator delete(void* p, const std::nothrow_t&) noexcept {
            operator delete(p);
        }

        static void operator delete(void* p, std::size_t) noexcept {
            operator delete(p);
        }

        // Prevent array new/delete — pooling is for single objects.
        static void* operator new[](std::size_t) = delete;
        static void operator delete[](void*) = delete;
    };

} // namespace access
} // namespace lease

#endif // LEASE_POOL_HPP
