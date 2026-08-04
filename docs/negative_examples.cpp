// negative_examples.cpp — what lease refuses to compile (and what it aborts on).
//
// This file intentionally compiles: every illegal program below lives inside
// an `#if 0` block so the rest of the project stays green. Uncomment any
// block, rebuild, and watch the compiler (or the Debug runtime) push back.
//
// The point is not the error messages. The point is *where* each illegal
// program dies:
//   - some are rejected by the type system  (no member, deleted function);
//   - some are rejected by Debug contract checking (abort, with a message).
// lease moves as much as possible to the first category, because a program
// that does not compile cannot ship.

#include "lease.hpp"

#include <memory>
#include <utility>

using lease::access::make_rw;
using lease::access::make_ro;
using lease::access::read_lock;
using lease::access::shared_access;
using lease::access::exclusive_access;

struct widget {
    int value = 0;
    void set(int v) noexcept { value = v; }
    int get() const noexcept { return value; }
};

// ============================================================================
// 1. Copying write authority  — forbidden by the type system
// ============================================================================
// An exclusive_access is the *unique* write authority. If it could be copied,
// two owners would exist and "exclusive" would be a lie.
#if 0
void split_writer_by_copy() {
    widget w;
    auto rw = make_rw(w);
    auto rw2 = rw;                       // ERROR: use of deleted function
    //   'exclusive_access::exclusive_access(const exclusive_access&)'
    //   is explicitly deleted
    (void)rw2;
}
#endif

// ============================================================================
// 2. Assigning write authority  — forbidden by the type system
// ============================================================================
#if 0
void split_writer_by_assign() {
    widget w;
    auto rw = make_rw(w);
    auto rw2 = make_rw(w);
    rw2 = rw;                            // ERROR: use of deleted function
    //   'exclusive_access& operator=(const exclusive_access&)' deleted
}
#endif

// ============================================================================
// 3. Handing write authority to a second function  — runtime contract breach
// ============================================================================
// Moving *transfers* the authority; it does not duplicate it. The second
// use of a moved-from handle is caught by contract checking, never silent.
#if 0
void take_writer(exclusive_access<widget> rw) { rw->set(1); }

void use_spent_handle() {
    widget w;
    auto rw = make_rw(w);
    take_writer(std::move(rw));          // authority transferred here
    take_writer(std::move(rw));          // contract violation (any build):
    //   lease::access contract violation: dereferencing an empty
    //   exclusive_access   → abort
}
#endif

// ============================================================================
// 4. Implicit read from write authority  — forbidden by the type system
// ============================================================================
// rw -> ro is a *downgrade*, a deliberate one-way verb (borrow_ro / downgrade),
// never an implicit conversion. The type system refuses to guess.
#if 0
void implicit_downgrade() {
    widget w;
    auto rw = make_rw(w);
    shared_access<widget> ro = rw;       // ERROR: no viable conversion
    //   'shared_access' is a different type from 'exclusive_access';
    //   there is no converting constructor.
    (void)ro;
}
#endif

// ============================================================================
// 5. Re-locking an already-locked handle  — forbidden by the type system
// ============================================================================
// lock() exists only on bare recipes (SFINAE). A locked proxy has no lock()
// member at all, so the attempt fails before the body is even parsed.
#if 0
void relock() {
    widget w;
    auto rw = make_rw<read_lock>(w);
    auto again = rw.lock();              // ERROR: no matching function
    //   note: candidate template ignored: requirement
    //   '!contains<read_lock, ...>' was not satisfied
    (void)again;
}
#endif

// ============================================================================
// 6. Two roots for one object  — Debug contract check
// ============================================================================
// The single-lineage guarantee: one object may have exactly one live root.
// Debug builds track provenance in a global registry and abort on conflict.
#if 0
void double_root() {
    widget w;
    auto rw = make_rw<read_lock>(w);     // registers w as lineage root
    auto ro = make_ro<read_lock>(w);     // Debug: contract violation
    //   lease::access contract violation: referent already belongs to
    //   another live proxy lineage   → abort
    (void)rw;
    (void)ro;
}
#endif

// ============================================================================
// 7. Locking with a live orphan reader  — documented contract, not runtime
// ============================================================================
// A bare reader derived before lock() contributes no count to the locked
// protocol. Locking while one is alive would create an uncounted participant
// racing the new writer. This is a *documented* contract, not a runtime
// check: bare readers are never registered, so the lock path cannot detect
// them — that is the honest price of the zero-cost path. The caller owns the
// promise; Debug builds only re-arm provenance checking at lock() itself.
#if 0
void lock_with_live_orphan() {
    widget w;
    auto rw = make_rw(w);
    auto ro = rw.borrow_ro();            // bare reader, zero count
    // Documented contract: ro must be destroyed before lock().
    auto locked = rw.lock();             // compiles, runs — caller's promise
    (void)ro;
    (void)locked;
}
#endif

int main() { return 0; }
