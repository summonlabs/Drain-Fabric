# Concurrency, ownership, and deadlock audit

## Ownership

| State | Owner | Guard |
| --- | --- | --- |
| Topology, obligations, fence, evidence, authority, drains, sets, exceptions, decisions | `DrainEngine` | one non-recursive `std::mutex` |
| Listener socket, connection registry | `ControllerServer` | `registry_mutex_` |
| One session's socket and framing state | that `Connection` | `Connection::mutex` |
| `sink_` pointer | `DrainEngine` | engine mutex; the hook itself runs outside it |
| Snapshot bytes | `Persistence` | none needed: `save`/`load`/`verify` are const-correct and the caller serialises them |

`DrainEngine` is neither copyable nor movable. There is exactly one mutex, and
it is never taken recursively.

## Lock ordering

The only nesting that exists is:

```
registry_mutex_  ->  (released)  ->  Connection::mutex  ->  (released)  ->  engine mutex
```

No two locks are ever held at the same time except inside `reap_connections`,
where only `registry_mutex_` is held while destroying shared pointers. A
connection's mutex is acquired with `std::try_to_lock` and is dropped before any
engine call, so **no lock is ever held while calling into another subsystem**.
That removes the possibility of lock inversion: there is no cycle to invert.

## Callbacks beneath locks

The engine never invokes a caller-supplied hook while holding its mutex:

* `advance` collects `EvacuationRequest`, `RestorationRequest`, and
  `AdmissionReopenRequest` values under the lock, releases it, and only then
  calls the sink.
* `restore_drain` follows the same pattern.
* The reentrancy guard `DrainEngine::lock_held_by_this_thread()` reports whether
  the calling thread holds the engine mutex. Every sink used by the test suite
  asserts it is false; `tests/drain_concurrency_tests.cpp` fails if a hook is
  ever invoked under the lock, and `tests/support/fixture.hpp` counts such calls.

Because of this, a hook may call back into the engine — the reentrant-hook test
does exactly that from two threads at once.

## Read-to-write upgrades

There is no upgrade path: every mutating entry point takes the single mutex once,
performs its reads and writes under it, and returns. Query methods take the same
mutex and return copies, so a caller can never hold a reference into engine state
after the lock is released. `ObligationTable::find_mutable` returns a raw pointer
but is private to the engine and is only used under the lock.

## Event emission

Decisions are recorded into a bounded `std::deque` under the lock. Nothing is
emitted to a socket, a file, or a logger from inside the engine; the server
performs all I/O in its own call frames.

## Shutdown and cancellation

* `ControllerServer::stop_workers` sets the stop flag and joins every worker.
  Workers poll with a bounded `select` budget, so they observe the flag promptly;
  the listener is closed as well so a blocked accept returns.
* `ControllerServer::stop` closes the listener, closes every session, and clears
  the registry. It is idempotent.
* `ChildProcess` in the test support terminates its child on destruction, so a
  failing test cannot orphan a daemon or an agent.
* `DrainEngine::cancel_drain` never leaves a half-applied transition: the
  transition is validated first, and cancellation deliberately leaves the
  admission fence closed. Only `restore_drain` reopens it, under a fresh
  generation.
* `Runtime::stop` revokes every authority token before writing the final
  snapshot, so a cancelled or shut-down runtime cannot be resumed with the
  authority it had.

## Work and memory bounds

| Surface | Bound | Enforced by |
| --- | --- | --- |
| targets per request | `max_targets_per_request` (hard cap 256) | request validation |
| drains per set | `max_drains_in_set` (hard cap 256) | request validation |
| obligations | `max_obligations` (hard cap 1,000,000) | `ObligationTable` |
| dependencies per obligation | 64 | construction and recovery |
| evidence records | `max_evidence_records`; superseded records are retired first | `EvidenceLedger` |
| decisions | `max_decisions_retained` | decision ring |
| blockers per drain | `max_blockers_per_drain` | blocker normalisation |
| exception grants | `max_exception_extensions` | `grant_exception` |
| topology resources / paths / groups | 4096 / 16384 / 1024 | `Topology` |
| route enumeration | 8 routes, depth 16, 20,000 expansions | `enumerate_routes` |
| frame payload | `max_frame_bytes` (hard cap 16 MiB) | framing and decode |
| JSON depth / nodes / bytes | 32 / 262,144 / 8 MiB by default | `json_parse` |
| snapshot payload | `max_snapshot_bytes` (hard cap 256 MiB) | `Persistence::save` |
| connections / workers / pending work | `max_connections`, `max_worker_threads`, `max_pending_work` | `ControllerServer` |

Every externally derived size passes through `drain/checked.hpp`; nothing
allocates from an unchecked length.

## What was audited and found

1. **Callbacks under the lock** — the first implementation of `advance` dispatched
   requests inline. It was restructured into collect-then-dispatch before the
   concurrency tests were written; the reentrancy counter now proves it.
2. **Blocked oscillation** — a drain blocked at the removal step was found
   ping-ponging between `Blocked` and `Quiescing` because the blocked
   re-evaluation did not repeat the removal validation. Fixed by making
   `evaluate_quiescence` mirror the removal-step checks, so the blocker is
   stable and explainable.
3. **Cross-drain interference** — a cancelled drain could restore a target that a
   different drain had drained, and two drains could race for the same fence.
   Fixed with explicit removal ownership derived from the fence, and the
   one-owner-per-target rule.
4. **Restart tax** — recovery blocks transient drains, and one advance was spent
   clearing that block. Resuming now performs the waiting transition in the same
   pass.
