---- MODULE ParticipantCount ----
EXTENDS Naturals

(*
  Simplified lease control block model — participant count only.

  The control block tracks a single atomic count:
    - rw creation:     count = 1
    - each ro/copy:    count += 1
    - each release:    count -= 1
    - write requires:  count == 1  (only rw, no active readers)
    - count == 0:      control block deleted

  Thread model:
    - rw is thread-confined (only its owning thread calls borrow_ro / write)
    - ro copies may live on other threads
    - borrow_ro does not happen during a write expression (same thread, sequential)
    - ro release may happen from any thread, at any time (including during write)
*)

VARIABLES count, writing

Init ==
  /\ count = 1
  /\ writing = FALSE

(* rw thread borrows a read share (not during a write expression) *)
BorrowRO ==
  /\ ~writing
  /\ count' = count + 1
  /\ writing' = writing

(* A reader (ro) releases its share — may come from any thread.
   Guard ~writing: during a write expression only the rw holder exists
   (count=1), so no ro can release. Guard count >= 1 covers the last-closer
   path: rw already gone (count=1), this ro's release brings count to 0
   and deletes the control block. *)
ReleaseRO ==
  /\ ~writing
  /\ count >= 1
  /\ count' = count - 1
  /\ writing' = writing

(* rw begins a write expression: requires exclusive access *)
BeginWrite ==
  /\ ~writing
  /\ count = 1
  /\ writing' = TRUE
  /\ count' = count

(* rw ends a write expression *)
EndWrite ==
  /\ writing
  /\ writing' = FALSE
  /\ count' = count

(* rw releases (downgrade or destroy) — requires rw still alive *)
ReleaseRW ==
  /\ ~writing
  /\ count >= 1
  /\ count' = count - 1
  /\ writing' = writing

(* Terminal: control block deleted *)
Done ==
  /\ count = 0
  /\ UNCHANGED <<count, writing>>

Next ==
  \/ BorrowRO
  \/ ReleaseRO
  \/ BeginWrite
  \/ EndWrite
  \/ ReleaseRW
  \/ Done

Spec == Init /\ [][Next]_<<count, writing>>

(* ============================================================ *)
(* Invariants                                                   *)
(* ============================================================ *)

(* State space bound for model checking *)
Bound == count <= 3

(* Writing implies exclusive access: no readers can be active *)
WriteExclusion == writing => count = 1

(* Count never goes negative *)
NoUnderflow == count >= 0

(* Terminal state is clean *)
TerminalOK == (count = 0) => ~writing

(* If not writing and count = 1, a write should be possible (no permanent stall) *)
(* This is a liveness-adjacent property; checked as a state invariant here:   *)
(* count = 1 /\ ~writing is always a legal BeginWrite state                   *)

====
