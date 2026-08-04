# lease — Architecture

This document describes how the six core concepts relate. It is not an API
reference; it explains *why the pieces are where they are*.

## The six concepts

| Concept | What it is | Where it lives |
|---|---|---|
| **Resource** | The object `T` you want to protect. lease never copies or moves it — it only governs access. | caller's storage |
| **Policy** | The access mode: `ro_tag` (shared, read-only projection) or `rw_tag` (unique, mutable projection). | compile-time tag |
| **Facade** | `shared_access<T>` / `exclusive_access<T>` — what the user actually holds. Owns the root slot, projects `const T*` / `T*`, and implements the management verbs (`borrow_ro`, `lock`, `downgrade`, `clone`). | `lease_facade.hpp` |
| **Decorator** | A compile-time policy layer that wraps the chain below it via the `apply` seam. `read_lock` is the only built-in; users add their own. | `lease_decorators.hpp` |
| **Control Block** | `lineage_control` — the heap object holding the atomic admission word and the registry entry. One per legal lineage. | `lease_storage.hpp` |
| **Lease** | The semantic model itself: access is *borrowed*, not owned. Every handle either owns the root or holds a counted/uncounted share, and every share must be returned. | the whole library |

## The type chain (materialization)

A recipe `make_rw<read_lock>(w)` is a `type_list<read_lock>` that
materializes into a concrete impl type. User decorators list outside-in;
`track` and `object_storage` are mandatory and implicit:

```
exclusive_access<widget, type_list<read_lock>>
│  └─ lineage_root_slot                 (unique_ptr<lineage_control> — root ownership)
│  └─ read_lock_impl<Inner, rw_tag>     (recipe marker: drives locked writer path)
│       └─ track_impl<...>              (mandatory: anchors control pointer)
│            └─ object_storage<widget>  (terminal: owns the raw referent pointer)
```

A bare recipe `make_rw(w)` is the same chain minus `read_lock_impl` — and
minus the heap block, the atomics, and the registry entry. That is the whole
cost story in one diagram: **the decorator list IS the cost model.**

## Ownership and lifetime

```
        ┌──────────────────────────────┐
        │          Resource (T)        │   never copied, never moved
        └──────────────▲───────────────┘
                       │ raw pointer (stable)
        ┌──────────────┴───────────────┐
        │       object_storage<T>      │
        └──────────────▲───────────────┘
                       │ decorated by read_lock (optional) and track (mandatory)
        ┌──────────────┴───────────────┐
        │        proxy facade          │
        │  lineage_root_slot (unique_ptr) │── owns ──► ┌──────────────────────┐
        └──────────────────────────────┘              │   lineage_control    │
                                                      │ ┌──────────────────┐ │
                                                      │ │  access_state    │ │
                                                      │ │  word (atomic)   │ │
                                                      │ │  [writer|root|   │ │
                                                      │ │   reader count]  │ │
                                                      │ └──────────────────┘ │
                                                      │ registry entry      │
                                                      └──────────────────────┘
```

- The **root** facade owns the control block via `unique_ptr`.
- Derived readers keep only the **stable raw pointer** to the control block —
  they own no memory, they own a *lease*.
- When the root dies while readers live, it clears the `root` bit and
  releases the `unique_ptr`. The **last reader** (last-closer) observes
  `root_alive == false` and deletes the orphan. No `shared_ptr`, no
  reference counting — a single ownership handoff plus a protocol.
- `lock()` is the lazy-lineage seam: a bare root holds no control block at
  all; the shared era begins exactly when `lock()` allocates it.

## One read expression, step by step

```
ro->get()
 │
 ├─ ro holds a counted share: read_lock_impl::control_  (acquired at borrow)
 ├─ operator->  →  const_object()  →  track_impl  →  object_storage::const_object
 │      plain load of the stable pointer, then const T* projection
 └─ no writer can be in flight: the read_lock protocol guarantees it
```

## One write expression, step by step

```
rw->set(7)
 │
 ├─ write_arrow<T, locked> constructed
 │    ├─ writer_scope<locked> acquires first     (CAS: reader==0 && no writer)
 │    ├─ mutable_write_object() resolves referent INSIDE the lock
 │    │    └─ decorator write-side state (audit counters) is updated inside
 │    │       the protocol's happens-before chain — never before acquire
 │    └─ T* exposed → set(7) → plain store
 └─ scope destructor releases the writer (CAS)
```

Bare recipe: the same expression is `writer_scope<false>` — an empty guard.
`set(7)` compiles to a single store.

## Why the seam order matters

Base order is `lineage_root_slot, impl_type`. Destruction runs in reverse,
so the decorator chain (which holds the reader share and the raw control
pointer) dies **before** root ownership releases the control block. The
last-closer decision is therefore always made with a live chain.

## Policy routing

`ro_tag` / `rw_tag` are threaded through every layer (materializer,
decorator impls, facade). They decide:

- which `proxy` specialization is instantiated (`ro_tag` → const projection);
- how `read_lock_impl` behaves (ro: counted share; rw: pure marker);
- which verbs exist (`lock()` is SFINAE'd to bare `rw_tag` recipes).

The tag is part of the type, which is the entire point: **access mode is
compile-time visible, never a runtime flag.**
