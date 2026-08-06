---- MODULE ParticipantCountRefined ----
EXTENDS Naturals

(*
  Refined model with ghost state proving that a single atomic refcount
  refines the full authority state machine.

  Ghost variables (exist only in the proof, NOT in C++):
    rootAlive  — root_slot owns a reference to the control block
    rootMode   — "rw" (exclusive root) | "ro" (reader root) | "none"
    readers    — non-root ro participants
    writing    — a write expression is in progress

  Concrete variable (what C++ actually stores):
    count      — intrusive refcount on lineage_control

  C++ refcount semantics (intrusive ref counting):
    lineage_control ctor:   refcount = 1  (creator's reference)
    root_slot copy ctor:    refcount += 1 (acquire — derived ro)
    root_slot move ctor:    refcount unchanged (transfer — downgrade)
    root_slot dtor:         refcount -= 1 (release; 0 → self-delete)

  Refinement invariant:
    count = BoolToNat(rootAlive) + readers

  This captures exactly what C++ stores:
    - root_slot contributes 1 while alive (the initial refcount or a moved ref)
    - each non-root reader contributes 1 (acquired via root_slot copy ctor)
    - rootMode "ro" does NOT add an extra count — the root reference IS the
      ro's reference (intrusive: one object = one refcount)

  Authority lifecycle (C++ entry points):
    make_rw<read_lock>:  rootAlive=T, rootMode="rw", count=1
    make_ro<read_lock>:  rootAlive=T, rootMode="ro", count=1
    borrow_ro / ro copy: readers++, count++ (root_slot copy ctor acquires)
    downgrade:           rootMode "rw"->"ro", count unchanged (move, not copy)
    rw destroy:          rootAlive=F, rootMode="none", count--
    ro root destroy:     rootAlive=F, rootMode="none", count--
    non-root ro destroy: readers--, count--

  Monotonicity: rootMode transitions are "rw"->"ro"->"none" or "rw"->"none".
  No action reverses these. Pure-ro state (rootMode="ro") can never return
  to rw authority — this is structural, verified by TLC.
*)

VARIABLES count, rootAlive, rootMode, readers, writing

BoolToNat(b) == IF b THEN 1 ELSE 0

Init ==
  /\ rootAlive = TRUE
  /\ readers = 0
  /\ writing = FALSE
  /\ \/ /\ rootMode = "rw"
        /\ count = 1
     \/ /\ rootMode = "ro"
        /\ count = 1

(* Add a reader participant (borrow_ro from rw, or copy from any ro).
   Guard ~writing: the contract forbids creating readers during a write
   expression. In C++ this is enforced by is_exclusive() at write_arrow
   construction (always compiled — one atomic load); the model enforces
   it at every step. *)
AddReader ==
  /\ count > 0
  /\ ~writing
  /\ readers' = readers + 1
  /\ count' = count + 1
  /\ UNCHANGED <<rootAlive, rootMode, writing>>

BeginWrite ==
  /\ rootAlive
  /\ rootMode = "rw"
  /\ ~writing
  /\ readers = 0
  /\ writing' = TRUE
  /\ UNCHANGED <<count, rootAlive, rootMode, readers>>

EndWrite ==
  /\ writing
  /\ writing' = FALSE
  /\ UNCHANGED <<count, rootAlive, rootMode, readers>>

(* A non-root reader releases — count -= 1 *)
ReleaseRO ==
  /\ ~writing
  /\ readers > 0
  /\ readers' = readers - 1
  /\ count' = count - 1
  /\ UNCHANGED <<rootAlive, rootMode, writing>>

(* rw root destroyed — root_slot dtor calls release(): count -= 1 *)
ReleaseRW ==
  /\ rootAlive
  /\ rootMode = "rw"
  /\ ~writing
  /\ rootAlive' = FALSE
  /\ rootMode' = "none"
  /\ count' = count - 1
  /\ UNCHANGED <<readers, writing>>

(* rw downgrades to ro root — rw's reference transfers to ro.
   refcount unchanged: rw dies (-1), ro is born (+1), net 0.
   In C++: root_slot move ctor transfers the pointer (no acquire/release).
   rootMode "rw" -> "ro". *)
Downgrade ==
  /\ rootAlive
  /\ rootMode = "rw"
  /\ ~writing
  /\ rootMode' = "ro"
  /\ UNCHANGED <<count, rootAlive, readers, writing>>

(* ro root destroyed — root_slot dtor calls release(): count -= 1.
   Same as ReleaseRW: one reference, one release. *)
ReleaseRORoot ==
  /\ rootAlive
  /\ rootMode = "ro"
  /\ ~writing
  /\ rootAlive' = FALSE
  /\ rootMode' = "none"
  /\ count' = count - 1
  /\ UNCHANGED <<readers, writing>>

Done ==
  /\ ~rootAlive
  /\ rootMode = "none"
  /\ count = 0
  /\ readers = 0
  /\ ~writing
  /\ UNCHANGED <<count, rootAlive, rootMode, readers, writing>>

Next ==
  \/ AddReader
  \/ BeginWrite
  \/ EndWrite
  \/ ReleaseRO
  \/ ReleaseRW
  \/ Downgrade
  \/ ReleaseRORoot
  \/ Done

Spec == Init /\ [][Next]_<<count, rootAlive, rootMode, readers, writing>>

(* ============================================================ *)
(* Invariants                                                   *)
(* ============================================================ *)

Bound == count <= 5 /\ readers <= 3

(* Core theorem: C++'s single atomic refcount exactly tracks the abstract
   authority state. Every reachable state satisfies this equation.
   Intrusive refcount: one reference per proxy, no extra counting layers. *)
CountRefinement == count = BoolToNat(rootAlive) + readers

WritingImpliesRW == writing => rootMode = "rw"
WritingImpliesNoReaders == writing => readers = 0
NoUnderflow == count >= 0
NoReaderUnderflow == readers >= 0
TerminalOK == (count = 0) => (~rootAlive /\ rootMode = "none" /\ readers = 0 /\ ~writing)
RootRWImpliesAlive == rootMode = "rw" => rootAlive
RootROImpliesAlive == rootMode = "ro" => rootAlive

====
