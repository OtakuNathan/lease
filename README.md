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
> You say `shared` when you mean shared. You call `enable_shared()` when
> you cross into the multi-threaded world. Nothing is done for you silently,
> because silent behavior is how C++ gets you.

## Design philosophy

lease is **best-effort authority modeling**. It makes read/write authority a
first-class type, enforces what it can at compile time, checks what it can in
Debug, and charges nothing for what you don't ask for. But it cannot do what
only a compiler can do — it cannot ban raw pointer arithmetic, cannot prevent
`const_cast`, cannot stop you from keeping a `T*` next to the proxy. Those
powers belong to the language, not the library.

**Preconditions (caller's responsibility):**

- The referent object outlives all proxies that reference it.
- The referent's address is stable for the duration of every lease.
- All access to the referent goes through the same lease domain (one lineage
  per object).

**What lease guarantees (within a single domain):**

- Read/write authority is expressed in the type: `exclusive_access<T>` vs
  `shared_access<T>`.
- Read authority (`shared_access`) may be shared — copied freely.
- Write authority (`exclusive_access`) may only be transferred (move), never
  copied or rebound.
- Debug builds verify single-lineage provenance, catch use-after-invalidate,
  and detect orphan readers (bare `borrow_ro` alive during a write) via a
  Debug-only control block.
- The optional `shared` decorator provides an intrusive refcount model:
  one atomic counter tracks all live proxies (rw + ro copies). Writing
  requires refcount == 1 (only the rw holder); writing while readers are
  active is a contract violation (always checked — abort in both Debug
  and Release).
- Every additional protection is purchased explicitly via a decorator — the
  single-threaded bare path pays for nothing it does not use.

**What lease cannot do:**

- It cannot prevent the caller from holding a raw `T*` alongside the proxy and
  writing through it.
- It cannot enforce that all access goes through lease — the compiler grants
  raw access; the library cannot revoke it.
- It cannot guarantee lifetime if the caller violates the preconditions.

This is the honest contract: lease gives you the tools to express authority
correctly, and the compiler will catch the mistakes it can see. The rest is
discipline.

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
  Lifetime ends by last-closer (refcount reaches 0), not by a
  counter that everyone can bump.

| | `shared_ptr` | lease |
|---|---|---|
| What it tracks | lifetime | authority |
| Write access | everyone | only `exclusive_access` |
| Cost of a share | atomic RMW (always) | plain copy (bare) or atomic RMW (shared) |
| Who can delete | last shared_ptr | last participant (refcount → 0) |

## Why not a mutex wrapper?

Because a mutex is a *schedule*, not a *type*. Wrapping an object in a
mutex protects each access *moment*; it says nothing about **who may** do
anything, and it taxes every call site whether or not there is contention.

lease compiles the authority into the type and charges you only for what
you asked for:

| Path | What you pay |
|---|---|
| bare recipe | nothing — a store, a load, a member copy |
| `shared` recipe | one atomic RMW per participant add/remove; write is an always-compiled refcount check |

The same `rw->set(7)` expression compiles to a **single store** in both
bare and shared recipes (Release). The shared recipe adds an
always-compiled `refcount == 1` check (one atomic load) — no CAS, no
spin, no backoff. The count IS the exclusivity check.

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

    // Borrow a read share. In bare mode this is free (no atomics).
    // In Debug, ro MUST die before any write or enable_shared() — the
    // exclusivity check catches orphan readers. In Release, bare has no
    // control block; this is the caller's documented contract.
    {
        auto ro = rw.borrow_ro();
        assert(ro->get() == 42);
    }

    // Cross the boundary: enable_shared() starts the participant-count era
    // (heap control block, atomics, registry entry). Explicit, one-way,
    // irreversible.
    {
        auto shared_rw = lease::access::enable_shared(rw);
        shared_rw->set(7);

        auto shared_ro = shared_rw.borrow_ro();   // counted: refcount = 2
        assert(shared_ro->get() == 7);
        // shared_ro dies here → refcount back to 1
    }
    // shared root is released here; refcount → 0 → control block self-deletes

    // Multi-threaded from the start: say so explicitly.
    auto shared = lease::access::make_rw<lease::access::shared>(w);
    shared->set(9);
}
```

Function signatures carry the mode, so no `&`, `const&`, or `&&`:

```cpp
int read_total(shared_access<widget> ro);       // "I only read"
void write(exclusive_access<widget> rw);        // "I take the authority"
```

### Scoped read/write lambdas

For multi-step mutations (e.g. `std::sort`), use `.write(lambda)` to make
the write intent visible:

```cpp
rw.write([](std::vector<int>& v) {
    std::sort(v.begin(), v.end());    // safe: no readers can exist
});

int first = ro.read([](const std::vector<int>& v) {
    return v.front();                 // const T&, cannot outlive the call
});
```

The lambda receives `T&` (or `const T&` for read). **Contract:** aliases
to the referent (pointers, references, iterators) must not escape the
lambda scope. The library cannot prevent a caller from wrapping a
reference in a struct and storing it — this is a caller obligation, not
an enforced guarantee. `noexcept` propagates from the lambda.

### Unified write path

All write operations — `operator->`, `.write(lambda)`, `.assign()`, and
`operator=(T)` — go through a single admission gate (`write_arrow`):

```
operator->      ─┐
write(lambda)   ─┤
assign()        ─┼──→ write_arrow ──→ core_valid → exclusivity → decorator seam
operator=(T)    ─┘
```

The admission check runs in a fixed order:

1. **`core_valid()`** — spent-token check via `object_storage` directly
   (not through the decorator chain). A spent proxy aborts here.
2. **Exclusivity** — `refcount == 1` via `core_control()` (root_slot's
   authoritative pointer). Always compiled (one atomic load).
3. **Decorator seam** — `mutable_write_object()` is called only after
   admission passes, so decorator side effects (audit counters, etc.)
   never fire on a rejected write.

### Reentrancy contract

`borrow_ro()` must NOT be called from inside a write expression
(`operator->`, `.write(lambda)`, `.assign()`). The rw holder is the only
thread that can create the first ro, so no external thread can violate
this — but the rw holder itself can reenter. Doing so creates a reader
during a write, which the next `write_arrow` catches (`refcount > 1` →
abort). This is a contract violation, not a runtime-synchronized path.

### Container indexing

`shared_access<T>` automatically gets `operator[]` when T supports it —
no explicit decorator needed:

```cpp
auto rw = make_rw(vec);
auto ro = rw.borrow_ro();
assert(ro[0] == 42);              // auto-injected, forwards to vec::operator[]

auto mrw = make_rw(mymap);
auto mro = mrw.borrow_ro();
assert(mro["key"] == 1);          // forwards to map::at() — const-safe, no silent insert
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
| read: lease bare | **1.7** |
| read: shared_ptr copy | 15.9 |
| read: lease shared | 38.5 |
| read: std::shared_mutex | 49.4 |
| write: lease bare | **1.2** |
| write: lease shared | **8.3** |
| write: std::shared_mutex | 70.2 |
| root create+destroy: lease | **38.9** |
| root create+destroy: shared_ptr | 55.8 |

The bare path is within noise of a bare reference. The shared write path is
**8.3 ns** — 8× faster than `std::shared_mutex` — because the intrusive refcount
model has no CAS loop, no spin, no backoff. The count IS the exclusivity check:
`refcount == 1` means exclusive, always checked.

Root creation+destruction is **38.9 ns** — faster than `std::shared_ptr`
(55.8 ns) — because `lineage_control` uses a pooled allocator (thread-local
cache → lock-free slab → malloc fallback) borrowed from flux_foundry.

### Debug checking is real, and it stays out of the Release hot path

`LSE_ACCESS_CHECKING` (on unless `NDEBUG`) tracks root provenance in a
global registry (single-lineage guarantee) and gives bare recipes a
control block for orphan-reader detection:

| case | release | debug |
|---|---|---|
| root create + destroy | 38.9 ns | 1517 ns |
| read: lease bare | 1.7 ns | 140.2 ns |
| read: lease shared | 38.5 ns | 148.7 ns |
| write: lease bare | 1.2 ns | 58.9 ns |
| write: lease shared | 8.3 ns | 58.6 ns |

In Debug, bare recipes now pay the same control-block cost as shared recipes
(orphan-reader detection). **Release is unchanged**: bare stays zero-cost
(no control block, no atomics), and only shared recipes check exclusivity.
The Debug-only cost is the root_registry provenance check + the control
block that bare now carries for contract enforcement.

## What the compiler refuses (read this, it's the best part)

The library's real beauty is not what it allows — it is what it **will not
compile**, and what it aborts on in Debug. Full walkthrough with real error
messages in [`docs/negative_examples.cpp`](docs/negative_examples.cpp).

```cpp
auto rw2 = rw;              // ERROR: deleted — write authority is unique
auto rw2 = std::move(rw);   // OK: transferred, then rw is spent
shared_access<widget> ro = rw;   // ERROR: no implicit downgrade
auto again = enable_shared(shared_rw);   // ERROR: static_assert — already shared
```

And at runtime (always checked, both Debug and Release):

```cpp
make_rw<shared>(w); make_ro<shared>(w);   // abort: two roots, one object (Debug only)
rw->set(1);  rw->set(1);  // after std::move(rw): abort: spent handle used
rw->set(1);               // after borrow_ro(): abort: write while readers active
```

A program that cannot compile cannot ship. The more lease moves into the
type system, the less it needs to check at runtime — and what remains is
checked loudly, with a message, never silently.

## Formal verification

The intrusive refcount model is verified with TLA+ (see `tla/ParticipantCount.tla`
and `tla/ParticipantCountRefined.tla`). TLC checks invariants over all
reachable states:

- **WriteExclusion**: `writing => count = 1` — a write expression implies
  exclusive access.
- **NoUnderflow**: `count >= 0` — the participant count never goes negative.
- **TerminalOK**: `count = 0 => ~writing` — the terminal state is clean.

```
Model checking completed. No error has been found.
  5 distinct states found, 0 states left on queue.
```

## The ideas behind it

- [`docs/architecture.md`](docs/architecture.md) — how Facade, Decorator,
  Lease, Control Block, Resource, and Policy relate, with the ownership and
  lifetime diagrams.
- [`docs/negative_examples.cpp`](docs/negative_examples.cpp) — illegal
  programs, compile-time and runtime, with the real errors they produce.
- [`bench/bench.cpp`](bench/bench.cpp) — the measured cost model above.
- [`tla/ParticipantCount.tla`](tla/ParticipantCount.tla) — TLA+ spec and
  TLC verification of the intrusive refcount model.
- [`tla/ParticipantCountRefined.tla`](tla/ParticipantCountRefined.tla) —
  Refined model with ghost state proving the refcount refines the full
  authority state machine.

## Layout

```
lease.hpp                 facade header — the only public entry point
lease_type_list.hpp       compile-time type-list IR (flat inheritance, O(1)
                          element_at — borrowed from dynabridge/type_list.h)
lease_storage.hpp         layer 1: intrusive refcount control block, track
lease_decorators.hpp      layer 2: shared, indexed, decorator contract probe
                          (the extension point)
lease_facade.hpp          layer 3: proxies, factories, write_arrow
tests/                    contract tests and examples
docs/                     architecture, negative examples
bench/                    the cost model, measured
tla/                      TLA+ spec and verification
```

## Requirements

- C++14 or newer (benchmark uses C++17 for `std::shared_mutex`)
- Standard library only — no Boost, no external dependencies
- CMake 3.20+ optional; every header works standalone
- Java 11+ + [TLA+ tools](https://github.com/tlaplus/tlaplus) optional; only
  needed to re-run formal verification
