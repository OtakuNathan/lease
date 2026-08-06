// lease.hpp — C++14 object capability + access control library (facade header).
//
// Public model
// ------------
//   T                         : value / ownership
//   make_ro<D...>(object)     : shared read capability for this object
//   make_rw<D...>(object)     : unique write authority for this object
//
// The proxy only governs the referent object itself (shallow constness).
// If T is a span/container/view, the semantics of the objects reachable through T
// remain T's responsibility. The proxy does not understand size/split/subspan/etc.
//
// Locking is a recipe decision, not a facade built-in. Explicit beats implicit:
//   - a bare recipe (no read_lock) is single-threaded and free: copies need no
//     counting and writes bypass the atomic word entirely;
//   - make_ro<read_lock>/make_rw<read_lock> arms the shared era: reader copies
//     count atomically and writer expressions use CAS exclusion;
//   - an exclusive_access without read_lock may lock() into a locked one; a
//     locked exclusive_access can never downgrade (no conversion exists).
//
// Lazy lineage (shared tax vs. existence tax):
//   - read_lock roots create lineage_control (heap block + registry entry) at
//     make_* time; bare roots create nothing — a bare referent only;
//   - the shared era begins exactly at lock(): that call allocates the
//     control block, registers the lineage, and starts the reader/writer
//     protocol. Single-thread code never pays for sharing it does not use.
//
// Layer structure (all layers live in namespace lease::access):
//   lease_storage.hpp     — raw storage, lineage control, mandatory track
//   lease_decorators.hpp  — optional decorators + decorator contract probe
//                               (this is the EXTENSION POINT of the library)
//   lease_facade.hpp      — proxy facades, factories, writer exclusion
//
// Internally a bare recipe is materialized as:
//   track_impl<object_storage<T>, Policy>
// and a user recipe such as make_rw<read_lock>(object) as:
//   read_lock_impl<track_impl<object_storage<T>, Policy>, Policy>
//
// C++14 self-contained. Single header (umbrella), zero external dependencies.
//
// Lifetime revision:
//   - each root proxy initially owns lineage_control with std::unique_ptr
//   - derived/read proxies keep a raw stable control pointer
//   - root destruction clears root_alive; if readers remain, the last reader
//     becomes the final closer and deletes the orphaned control block
//   - no std::shared_ptr/refcount is required

#ifndef LEASE_HPP
#define LEASE_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
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
#include <vector>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

#if defined(NDEBUG)
#  define LSE_ACCESS_CHECKING 0
#else
#  define LSE_ACCESS_CHECKING 1
#endif

// ============================================================
// Language-version and force-inline macros, borrowed from
// flux_foundry/base/traits.h (FORCE_INLINE / FLUX_FOUNDRY_CPP_AT_LEAST)
// and renamed with the LSE_ prefix so this TU can coexist with flux_foundry.
//
// _MSVC_LANG is checked first: MSVC reports __cplusplus as 199711L unless
// /Zc:__cplusplus is passed, so a raw __cplusplus test silently disables
// C++20 features there.
// ============================================================
#define LSE_CPP_14 201402L
#define LSE_CPP_17 201703L
#define LSE_CPP_20 202002L

#if defined(_MSVC_LANG)
#  define LSE_CPP_VER _MSVC_LANG
#elif defined(__cplusplus)
#  define LSE_CPP_VER __cplusplus
#else
#  define LSE_CPP_VER 0L
#endif

#define LSE_CPP_AT_LEAST(ver) (LSE_CPP_VER >= LSE_CPP_##ver)
#define LSE_CPP_AT_MOST(ver)  (LSE_CPP_VER <= LSE_CPP_##ver)

#if defined(__GNUC__) || defined(__clang__)
#  define LSE_FORCE_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define LSE_FORCE_INLINE __forceinline
#else
#  define LSE_FORCE_INLINE inline
#endif

// Branch-prediction hints, flux_foundry LIKELY_IF style:
// C++20 standard attributes when available, GCC/Clang builtins otherwise.
// Keep the guards; this only helps the branch predictor on hot paths.
#if LSE_CPP_AT_LEAST(20)
#  define LSE_LIKELY_IF(expr) if ((expr)) [[likely]]
#  define LSE_UNLIKELY_IF(expr) if ((expr)) [[unlikely]]
#elif defined(__GNUC__) || defined(__clang__)
#  define LSE_LIKELY_IF(expr) if (__builtin_expect(!!(expr), 1))
#  define LSE_UNLIKELY_IF(expr) if (__builtin_expect(!!(expr), 0))
#else
#  define LSE_LIKELY_IF(expr)   if (expr)
#  define LSE_UNLIKELY_IF(expr) if (expr)
#endif

namespace lease {

// ============================================================
// Minimal C++14 span polyfill.
// It is a normal resource type. The proxy has no span-specific code.
// const span<T> is intentionally shallow const: operator[] const returns T&.
// ============================================================
    template <typename T>
    class span {
        static_assert(!std::is_reference<T>::value,
                      "span element type must not be a reference");

    public:
        using element_type = T;
        using value_type = typename std::remove_cv<T>::type;
        using pointer = T*;
        using reference = T&;
        using size_type = std::size_t;
        using iterator = pointer;

        constexpr span() noexcept : data_(nullptr), size_(0) {}

        constexpr span(pointer data, size_type size) noexcept
                : data_(data), size_(size) {
            assert((data_ != nullptr || size_ == 0) &&
                   "non-empty span requires non-null data");
        }

        template <std::size_t N>
        constexpr span(element_type (&array)[N]) noexcept
                : data_(array), size_(N) {}

        template <typename U,
                typename std::enable_if<
                        std::is_convertible<U (*)[], T (*)[]>::value,
                        int>::type = 0>
        constexpr span(const span<U>& rhs) noexcept
                : data_(rhs.data()), size_(rhs.size()) {}

        constexpr pointer data() const noexcept { return data_; }
        constexpr size_type size() const noexcept { return size_; }
        constexpr bool empty() const noexcept { return size_ == 0; }

        // Shallow const: descriptor constness does not recursively freeze elements.
        constexpr reference operator[](size_type index) const noexcept {
            assert(index < size_ && "span index out of range");
            return data_[index];
        }

        constexpr iterator begin() const noexcept { return data_; }
        constexpr iterator end() const noexcept { return data_ + size_; }

        constexpr span first(size_type count) const noexcept {
            assert(count <= size_ && "span::first out of range");
            return span(data_, count);
        }

        constexpr span last(size_type count) const noexcept {
            assert(count <= size_ && "span::last out of range");
            return span(data_ + (size_ - count), count);
        }

        constexpr span subspan(size_type offset) const noexcept {
            assert(offset <= size_ && "span::subspan out of range");
            return span(data_ + offset, size_ - offset);
        }

        constexpr span subspan(size_type offset, size_type count) const noexcept {
            assert(offset <= size_ && count <= size_ - offset &&
                   "span::subspan out of range");
            return span(data_ + offset, count);
        }

        // Destructive descriptor split. This is span semantics, not proxy semantics.
        span split_off(size_type offset) noexcept {
            assert(offset <= size_ && "span::split_off out of range");
            span tail(data_ + offset, size_ - offset);
            size_ = offset;
            return tail;
        }

    private:
        pointer data_;
        size_type size_;
    };

} // namespace lease

// Layer 0: pooled allocation for lineage_control (borrowed from flux_foundry).
#include "lease_pool.hpp"
// Layer 1: raw storage, lineage control, mandatory track layer.
#include "lease_storage.hpp"
// Layer 2: optional decorators + decorator contract probe (extension point).
#include "lease_decorators.hpp"
// Layer 3: proxy facades, factories, writer exclusion.
#include "lease_facade.hpp"

#endif // LEASE_HPP
