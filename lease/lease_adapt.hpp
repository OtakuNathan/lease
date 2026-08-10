// lease_adapt.hpp — Unified access/value adapters for generic code.
//
// Internal header: include via lease.hpp only.
//
// Two operations, each overloaded for value, raw pointer, and capability:
//
//   access(x) — in-place access. Returns a pointer (value/raw-ptr) or the
//               capability itself (which has operator->). The return type
//               encodes authority:
//                 mutable value    → T*
//                 const value      → const T*
//                 raw ptr          → T*        (escape hatch, full authority)
//                 shared_access    → Cap&      → operator-> → const T*
//                 exclusive_access → Cap&      → operator-> → T* (write_arrow)
//
//   value(x)  — extract T by value. For value/raw-ptr: pass-through
//               (copy elision / implicit move). For capabilities: clone().
//
// Design rationale:
//   C++ cannot overload operator., so all in-place access is unified through
//   operator->. access() normalizes value/ptr/cap into something that has
//   operator->; the compiler selects the overload and the return type IS the
//   authority. No trait struct, no wrapper class — just function overloads.
//
//   value() normalizes "give me a T" across storage forms: move for value,
//   clone for cap. Generic code (e.g. constructors) takes T and calls value(x)
//   without knowing or caring whether x was a cap or a plain value.

#ifndef LEASE_ADAPT_HPP
#define LEASE_ADAPT_HPP

#include <type_traits>

#include "lease_facade.hpp"

namespace lease {
namespace access {

// ============================================================
// is_capability: detects any proxy specialization
// (shared_access / exclusive_access, with or without decorators).
// Cv-qualifiers are stripped so const proxy<...> is also detected.
// ============================================================
        template <typename T>
        struct is_capability : std::false_type {};

        template <typename T, typename DL, typename Policy>
        struct is_capability<proxy<T, DL, Policy>> : std::true_type {};

// ============================================================
// access — unified in-place access via operator->.
// ============================================================

        // Mutable non-cap value → T* (full authority over your own copy).
        template <typename T,
                  std::enable_if_t<!is_capability<std::remove_cv_t<T>>::value, int> = 0>
        T* access(T& v) noexcept {
            return std::addressof(v);
        }

        // Const non-cap value → const T* (read-only).
        template <typename T,
                  std::enable_if_t<!is_capability<std::remove_cv_t<T>>::value, int> = 0>
        const T* access(const T& v) noexcept {
            return std::addressof(v);
        }

        // Raw pointer → passthrough (escape hatch, full authority).
        // More specialized than the T& overload, so pointers land here.
        template <typename T>
        T* access(T* p) noexcept {
            return p;
        }

        // Capability → passthrough. The cap already has operator->;
        // its return type carries the authority (const T* for shared,
        // write_arrow<T> for exclusive). Cv-qualifiers are stripped in
        // the SFINAE check so const capabilities are detected correctly.
        template <typename Cap,
                  std::enable_if_t<is_capability<std::remove_cv_t<Cap>>::value, int> = 0>
        Cap& access(Cap& cap) noexcept {
            return cap;
        }

// ============================================================
// value — unified value extraction.
// ============================================================

        // Non-cap value → pass-through by value (copy elision / implicit move).
        template <typename T,
                  std::enable_if_t<!is_capability<std::remove_cv_t<T>>::value, int> = 0>
        T value(T v) {
            return v;
        }

        // proxy<T, DL, ro_tag> (shared_access) → clone through the cap's
        // own method. Matches on the materialized proxy type, not the alias,
        // so decorated capabilities (where D... passes through dedup_t)
        // deduce correctly.
        template <typename T, typename DL>
        T value(const proxy<T, DL, ro_tag>& cap) {
            return cap.clone();
        }

        // proxy<T, DL, rw_tag> (exclusive_access) → clone.
        template <typename T, typename DL>
        T value(const proxy<T, DL, rw_tag>& cap) {
            return cap.clone();
        }

} // namespace access
} // namespace lease

#endif // LEASE_ADAPT_HPP
