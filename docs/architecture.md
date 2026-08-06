# lease — Architecture

This document describes how the core concepts relate. It is not an API
reference; it explains *why the pieces are where they are*.

## The concepts

| Concept | What it is | Where it lives |
|---|---|---|
| **Resource** | The object `T` you want to protect. lease never copies or moves it — it only governs access. | caller's storage |
| **Policy** | The access mode: `ro_tag` (shared, read-only projection) or `rw_tag` (unique, mutable projection). | compile-time tag |
| **Facade** | `shared_access<T>` / `exclusive_access<T>` — what the user actually holds. Inherits root slot, projects `const T*` / `T*`, and implements the management verbs (`borrow_ro`, `downgrade`, `clone`, `write`, `read`). | `lease_facade.hpp` |
| **Decorator** | A compile-time policy layer that wraps the chain below it via the `apply` seam. `read_lock` is the only built-in; users add their own (`indexed`, `audit`, etc.). | `lease_decorators.hpp` |
| **Control Block** | `lineage_control` — the heap object holding the intrusive refcount and the Debug provenance entry. One per locked lineage. | `lease_storage.hpp` |
| **Lease** | The semantic model itself: access is *borrowed*, not owned. Every handle either owns the root reference or holds a counted share, and every share must be returned. | the whole library |

## The type chain (materialization)

A recipe `make_rw<read_lock>(w)` is a `type_list<read_lock>` that
materializes into a concrete impl type. User decorators list outside-in;
`track` and `object_storage` are mandatory and implicit:

```
exclusive_access<widget, type_list<read_lock>>
│  └─ lineage_root_slot              (raw pointer to lineage_control — RAII reference)
│  └─ read_lock_impl<Inner, rw_tag>  (recipe marker: drives locked write path)
│       └─ track_impl<...>           (mandatory: anchors observational control pointer)
│            └─ object_storage<widget> (terminal: owns the raw referent pointer)
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
        │  lineage_root_slot           │── references ──► ┌──────────────────────┐
        │    (raw ptr + RAII)          │                  │   lineage_control    │
        └──────────────────────────────┘                  │ ┌──────────────────┐ │
                                                          │ │  refcount        │ │
                                                          │ │  (atomic uint64) │ │
                                                          │ └──────────────────┘ │
                                                          │ Debug provenance     │
                                                          └──────────────────────┘
```

- The **root** facade holds a raw pointer to `lineage_control`. The pointer
  is set at construction (inheriting the initial refcount=1) and released in
  the destructor (refcount -= 1; if 0, self-deletes).
- Derived readers (borrow_ro, ro copy) acquire a new reference via the
  `lineage_root_slot` copy constructor: `control_->acquire()` (+1).
- Destructive transfers (downgrade, enable_locking) move the pointer without
  acquire/release — refcount unchanged.
- When refcount reaches 0, `lineage_control` self-deletes and cleans up the
  Debug provenance entry. No `shared_ptr`, no external refcount — the control
  block IS the refcount.
- `enable_locking()` is the lazy-lineage seam: a bare root holds no control
  block at all; the shared era begins exactly when `enable_locking()` allocates it.

## The intrusive refcount model

The control block is born with `refcount = 1` (the creator's reference).
Every proxy that holds a reference calls `acquire()` (+1) on copy and
`release()` (-1) on destruction. When refcount reaches 0, the control block
deletes itself.

Write authority: `refcount == 1` means only the rw holder exists — exclusive
by construction. `refcount > 1` means readers are active → contract violation.

No writer bit, no root bit, no CAS loop, no backoff. **The count IS the lock.**

### Authority lifecycle

| Operation | refcount change | How |
|---|---|---|
| `make_rw<read_lock>` / `make_ro<read_lock>` | = 1 | `new lineage_control` (ctor sets refcount=1) |
| `enable_locking(bare)` | = 1 | same: fresh control block |
| `borrow_ro` / ro copy | +1 | `root_slot` copy ctor → `acquire()` |
| `downgrade` | unchanged | `detach_control()` — move, not copy |
| rw destroy | -1 | `~root_slot` → `release()` |
| ro root destroy | -1 | same |
| non-root ro destroy | -1 | same |
| refcount → 0 | self-delete | `release()` detects last reference |

## One read expression, step by step

```
ro->get()
 │
 ├─ ro holds a reference: lineage_root_slot::control_  (acquired at borrow)
 ├─ operator->  →  const_object()  →  track_impl  →  object_storage::const_object
 │      plain load of the stable pointer, then const T* projection
 └─ no writer can be in flight: refcount > 1 means readers exist,
    and write_arrow checks refcount == 1 before admitting any write
```

## One write expression, step by step

```
rw->set(7)
 │
 ├─ write_arrow<T, locked> constructed
 │    ├─ Step 1: core_valid() — spent-token check via object_storage
 │    │    directly (NOT through decorator chain). A spent proxy aborts here.
 │    ├─ Step 2: exclusivity check via core_control() (root_slot's
 │    │    authoritative pointer). refcount == 1 required. Always compiled.
 │    └─ Step 3: mutable_write_object() — decorator write seam, called
 │         ONLY after admission passes. Side effects (audit counters, etc.)
 │         never fire on a rejected write.
 ├─ T* exposed → set(7) → plain store
 └─ write_arrow destructor (trivial — no scope to release)
```

Bare recipe: `write_arrow<T, false>` — Step 2 is skipped (no control block).
`set(7)` compiles to a single store.

### Unified write path

All write operations go through `write_arrow`:

```
operator->      ─┐
write(lambda)   ─┤
assign()        ─┼──→ write_arrow ──→ core_valid → exclusivity → decorator seam
operator=(T)    ─┘
```

No write can bypass admission. The decorator seam (`mutable_write_object`)
is reached only after admission passes.

## Core authority vs. decorator observation

There are two control pointers in the chain:

| Pointer | Owner | Purpose | Used by |
|---|---|---|---|
| `lineage_root_slot::control_` | root slot (base class) | **Authoritative**: acquire/release/detach | write_arrow, borrow_ro, downgrade |
| `track_impl::control_` | track decorator | **Observational**: audit, diagnostics | user decorators, `control_pointer()` |

Core operations (`write_arrow`, `borrow_ro`, `downgrade`) use
`core_control()` / `detach_control()` from `lineage_root_slot` — they never
go through the decorator chain. A malicious decorator that hides
`control_pointer()` cannot affect authority decisions.

## Why the seam order matters

Base order is `lineage_root_slot, impl_type`. Destruction runs in reverse,
so the decorator chain dies **before** `root_slot` releases its reference.
This ensures the decorator chain's observational pointers are never used
after the control block has been released.

## Policy routing

`ro_tag` / `rw_tag` are threaded through every layer (materializer,
decorator impls, facade). They decide:

- which `proxy` specialization is instantiated (`ro_tag` → const projection);
- how `read_lock_impl` behaves (ro: pure marker; rw: pure marker — refcount
  is managed by `lineage_root_slot`, not by `read_lock`);
- which verbs exist (`enable_locking()` is `static_assert`'d to bare
  `rw_tag` recipes).

The tag is part of the type, which is the entire point: **access mode is
compile-time visible, never a runtime flag.**

## Formal verification

The intrusive refcount model is verified with TLA+ (see
`tla/ParticipantCount.tla` and `tla/ParticipantCountRefined.tla`).

- **ParticipantCount.tla** — base model: refcount state machine with
  WriteExclusion, NoUnderflow, and TerminalOK invariants.
- **ParticipantCountRefined.tla** — refined model with ghost state
  (`rootAlive`, `rootMode`, `readers`, `writing`) proving that the single
  atomic refcount refines the full authority state machine.

The refinement invariant: `count = BoolToNat(rootAlive) + readers`.
