# lease

A C++14, header-only library that lets you say *who may read* and *who may
write* an object — in the **type**, not in comments, not at runtime.

```cpp
auto rw = lease::access::make_rw(w);   // I hold the write authority
auto ro = rw.borrow_ro();              // I borrowed a read share
```

That is the whole pitch. Everything else in this document answers one
question: **why does this need to exist?**

---

## Why lease?

C++ gives you four ways to pass an object around. All four answer *different*
questions, and none of them answers the one lease answers:

| Tool | Answers | Does NOT answer |
|---|---|---|
| `T&` / `const T&` | "where is it?" | lifetime, concurrency, *who can write* |
| `std::shared_ptr<T>` | "who keeps it alive?" | *who can write* |
| `std::mutex` / `std::shared_mutex` | "when is it safe to touch?" | lifetime, *who can write* |
| Rust's borrow checker | "who can write?" | — (but it's not C++) |

Every one of the first three silently grants **write access to everyone who
can see the object**. A `shared_ptr<T>` in a struct means every member
function can mutate `T`. A `shared_mutex` guards the *moment*, not the
*right*: any caller that grabs the lock can write. Rust is the only one that
makes "who may write" a compile-time fact — and you are not writing Rust.

**lease is the C++ answer to the borrow checker: access authority as a
first-class type.** `exclusive_access<T>` and `shared_access<T>` are not
wrappers that add checks; they are *different types*. The compiler enforces
the same rules Rust enforces — no copying write authority, no upgrading
read authority, no silent races — at zero runtime cost on the path that
does not need it.

The design principle is the same one that runs through everything here:

> **Explicit beats implicit.** A bare recipe is single-threaded and free.
> You say `read_lock` when you mean shared. You call `lock()` when you cross
> into the multi-threaded world. Nothing is done for you silently, because
> silent behavior is how C++ gets you.

## Why not `T&` / `const T&`?

Because references are zero-cost and zero-promise.

- `const T&` can bind to a **temporary** — and if anyone stores that
  reference, it dangles the moment the full-expression ends. The compiler
  said "fine", and then you have UB.
- `const T&` promises "I will not write" — and then `const_cast` exists,
  and `mutable` exists, and the reference can be stashed in a field and
  handed to another thread.
- Nothing in a reference signature says anything about **lifetime** or
  **concurrency**. `void f(const T&)` tells you "maybe reads", and nothing
  else.

lease replaces the *position* of the reference with the *mode* of access.
The signature `void f(shared_access<T> ro)` says "I can read, I hold a
lease, and I die with it." One type, three facts.

## Why not `std::shared_ptr<T>`?

Because shared_ptr answers "who keeps it alive?" and lease answers "who may
touch it?" — and conflating the two is how ownership bugs happen.

- A `shared_ptr<T>` **gives every holder write access**. There is no
  `shared_ptr<const T>` discipline that survives being passed around.
- Reference counting is about *lifetime*, so it keeps counting long after
  the interesting question became "is a writer in flight?"
- lease separates the two cleanly: the **root** owns the control block
  (lifetime), and the **lease** carries the access mode (authority).
  Lifetime ends by last-closer, not by a counter that everyone can bump.

| | `shared_ptr` | lease |
|---|---|---|
| What it tracks | lifetime | authority |
| Write access | everyone | only `exclusive_access` |
| Cost of a share | atomic RMW (always) | plain copy (bare) or atomic RMW (locked) |
| Who can delete | last shared_ptr | root, or last reader by protocol |

## Why not a mutex wrapper?

Because a mutex is a *schedule*, not a *type*. Wrapping an object in a
mutex protects each access *moment*; it says nothing about **who may** do
anything, and it taxes every call site whether or not there is contention.

lease compiles the exclusion into the type and charges you only for what
you asked for:

| Path | What you pay |
|---|---|
| bare recipe | nothing — a store, a load, a member copy |
| `read_lock` recipe | one atomic RMW per borrow, CAS per write |

The same `rw->set(7)` expression compiles to a **single store** in a bare
recipe and a **CAS protocol** in a locked one — chosen at compile time, by
the type, not by a runtime lock we carry everywhere "just in case."

## Quick start

```cpp
#include <lease.hpp>

struct widget {
    int value = 0;
    void set(int v) noexcept { value = v; }
    int get() const noexcept { return value; }
};

int main() {
    widget w;

    // Bare recipe: single-threaded, free. The default.
    auto rw = lease::access::make_rw(w);
    rw->set(42);
    assert(w.value == 42);

    auto ro = rw.borrow_ro();            // a read share; must die before lock()
    assert(ro->get() == 42);

    // Cross the boundary: lock() starts the shared era (heap control block,
    // atomics, registry entry). Explicit, one-way, irreversible.
    {
        auto locked = rw.lock();
        locked->set(7);
    }   // locked root is released here; one lineage per object

    // Multi-threaded from the start: say so explicitly.
    auto shared = lease::access::make_rw<lease::access::read_lock>(w);
    shared->set(9);
}
```

Function signatures carry the mode, so no `&`, `const&`, or `&&`:

```cpp
int read_total(shared_access<widget> ro);       // "I only read"
void write(exclusive_access<widget> rw);        // "I take the authority"
```

## The cost model, measured

`bench/bench.cpp` — build with `g++ -std=c++17 -O2 -DNDEBUG -Ilease
bench/bench.cpp -pthread`. Numbers are ns/op, relative, on one machine.
The *shape* is what matters:

> **Hardware**: Raspberry Pi 4 Model B (ARM Cortex-A72, aarch64, 4 cores,
> 4GB). This is a deliberately slow, low-power board — the bare-path numbers
> are the *worst case* a desktop x86 would show, and even here they are
> within noise of a bare reference.

| case | ns/op |
|---|---|
| read: lease bare | **1.1** |
| read: shared_ptr copy | 15.9 |
| read: lease read_lock | 41.4 |
| read: std::shared_mutex | 49.2 |
| write: lease bare | **1.1** |
| write: lease read_lock | 41.4 |
| write: std::shared_mutex | 77.3 |

The bare path is within noise of a bare reference. The locked path beats a
`shared_mutex` on both read and write — one packed atomic word vs. a mutex
state machine.

### Debug checking is real, and it stays out of the hot path

`LSE_ACCESS_CHECKING` (on unless `NDEBUG`) tracks root provenance in a
global registry. Its cost is a **root-lifecycle tax**, not a per-access tax:

| case | release | debug |
|---|---|---|
| root create + destroy | 104 ns | 246 ns |
| read: lease bare | 1.12 ns | 1.12 ns |
| read: lease read_lock | 41.4 ns | 41.0 ns |

Creating a root in Debug pays the global mutex + hash map entry; **the hot
paths are unchanged**. You pay for checking exactly where checking happens —
at the boundaries, not in the loop.

## What the compiler refuses (read this, it's the best part)

The library's real beauty is not what it allows — it is what it **will not
compile**, and what it aborts on in Debug. Full walkthrough with real error
messages in [`docs/negative_examples.cpp`](docs/negative_examples.cpp).

```cpp
auto rw2 = rw;              // ERROR: deleted — write authority is unique
auto rw2 = std::move(rw);   // OK: transferred, then rw is spent
shared_access<widget> ro = rw;   // ERROR: no implicit downgrade
auto again = locked.lock();      // ERROR: no such member (SFINAE)
```

And in Debug builds:

```cpp
make_rw<read_lock>(w); make_ro<read_lock>(w);   // abort: two roots, one object
rw->set(1);  rw->set(1);  // after std::move(rw): abort: empty handle used
```

A program that cannot compile cannot ship. The more lease moves into the
type system, the less it needs to check at runtime — and what remains is
checked loudly, with a message, never silently.

## The ideas behind it

- [`docs/architecture.md`](docs/architecture.md) — how Facade, Decorator,
  Lease, Control Block, Resource, and Policy relate, with the ownership and
  happens-before diagrams.
- [`docs/negative_examples.cpp`](docs/negative_examples.cpp) — illegal
  programs, compile-time and runtime, with the real errors they produce.
- [`bench/bench.cpp`](bench/bench.cpp) — the measured cost model above.

## Layout

```
lease.hpp                 facade header — the only public entry point
lease_type_list.hpp       compile-time type-list IR (flat inheritance, O(1)
                          element_at — borrowed from dynabridge/type_list.h)
lease_storage.hpp         layer 1: storage, lineage control, mandatory track
lease_decorators.hpp      layer 2: decorators + decorator contract probe
                          (the extension point)
lease_facade.hpp          layer 3: proxies, factories, writer exclusion
tests/                    contract tests and examples
docs/                     architecture, negative examples
bench/                    the cost model, measured
```

## Requirements

- C++14 or newer (benchmark uses C++17 for `std::shared_mutex`)
- Standard library only — no Boost, no external dependencies
- CMake 3.20+ optional; every header works standalone
