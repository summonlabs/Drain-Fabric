# Drain Fabric

**A vendor-neutral Fabric OS runtime for controlled resource drain.**
Summon Software Labs, 2026.

Drain Fabric owns one job: moving a network resource from serving to unavailable
without dropping anything it promised to keep. It closes admission, accounts for
every obligation the resource still carries, asks adjacent runtimes to move or
release that work, validates capacity and redundancy before each removal step,
and only then grants final drain authority.

It is not a maintenance tool, not a routing engine, and not an upgrade planner.

```text
        request drain
              |
              v
   Requested -> Validating -> AdmissionClosed -> Evacuating -> Quiescing -> Verifying -> Drained
                  |               |                |             |            |
                  |               |                |             |            +--> Blocked
                  |               |                |             +---------------> Cancelled
                  |               |                +-----------------------------> Restoring
                  |               +----------------------------------------------> Failed
                  +-------------------------------------------------------------->
```

## What is implemented

* **Typed domain identities.** `DrainTarget`, `DrainRecord`, `DrainSet`,
  `Obligation`, `Evidence`, `AuthorityToken`, and `Decision`, with distinct
  numeric types for generations, epochs, incarnations, authorities, revisions,
  attempts, sequences, and nonces, and distinct quantity types for capacity,
  counts, and byte budgets.
* **An explicit obligation model.** Active flows, reservations, workload network
  contracts, path-diversity commitments, capacity guarantees, and supplied
  dependencies, each with a protection class, an evacuation mode, a grace
  window, and a full state machine.
* **The drain lifecycle** with a legality table, a remembered state for
  `Blocked`, and a removal step that happens only after capacity, redundancy,
  diversity, and connectivity have been validated.
* **An admission fence** keyed by target and generation, so a caller that
  presents a superseded generation is fenced rather than silently admitted.
* **Evacuation hooks.** Drain Fabric *requests* reroute, migration, and release
  from adjacent runtimes through `EvacuationSink`; it never performs them.
* **Evidence fencing.** Per-stream monotonic sequences, a bounded clock-skew
  tolerance, quarantine on recovery, and freshness measured from the instant of
  observation. Stale evidence produces a blocker; it never completes a drain.
* **Deterministic blockers and explanations.** Twenty-one blocker codes naming
  the exact obligation, path, pool, or evidence stream that is holding a drain,
  plus a decision log recording inputs, evidence, policy revision and
  fingerprint, generation, authority, and rejected alternatives.
* **Grace and exceptions.** Policy deadlines never fail a drain; they produce
  blockers that an operator clears with a bounded exception grant.
* **Cancellation and restoration.** Cancellation stops the drain and keeps the
  fence closed. Restoration reopens admission under a strictly newer generation
  and refuses to touch a fence another drain owns.
* **Versioned, integrity-checked persistence** with atomic replacement, a single
  backup, a boot marker, conservative recovery, and fresh-incarnation fencing.
* **Correlated drain sets** with deterministic ordering, failure-domain-aware
  limits, and set-level validation against the commitments as if the whole set
  had already been applied.
* **A CLI, a controller daemon, and a node agent** that coordinate over real
  framed TCP on loopback.

## Quick start

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
./build/release/examples/drain_lifecycle_demo
```

Drive a daemon and an agent as separate processes:

```sh
./build/release/tools/drainfabricd --state /tmp/drain.state --listen 127.0.0.1:9100 --ready-file /tmp/drain.ready &
./build/release/tools/drainagent --connect 127.0.0.1:9100 --node node-1 \
    --obligations /tmp/node-1.json --ready-file /tmp/agent.ready &
```

Or use the operator CLI against durable state:

```sh
./build/release/tools/drainctl --state ./state.drainlog init
./build/release/tools/drainctl --state ./state.drainlog load-topology --file topology.json
./build/release/tools/drainctl --state ./state.drainlog request --target resource:leaf-1@pod-a
./build/release/tools/drainctl --state ./state.drainlog advance --steps 8
./build/release/tools/drainctl --state ./state.drainlog explain --drain 1
./build/release/tools/drainctl --state ./state.drainlog audit
```

## Invariants the runtime enforces

1. No new obligation binds after admission is authoritatively closed.
2. A resource is never declared drained while a protected obligation still
   depends on it.
3. Stale completion evidence cannot finish a drain.
4. Correlated drains cannot violate declared redundancy or failure-domain limits.
5. Cancellation cannot resurrect stale authority, and restoration only reopens
   admission under a fresh generation.
6. A drain may only claim a removal *it* performed.

Every one of them is asserted by the test suite, and the accounting audit
recomputes them from the authoritative snapshot on demand.

## Evidence of correctness

| Suite | What it proves |
| --- | --- |
| `drain_unit_tests` | component contracts, the lifecycle legality table, fence arithmetic, ledger fencing, package integrity |
| `drain_property_tests` | seeded randomized obligation graphs, topologies, and event sequences audited at every step; identical inputs produce byte-identical decisions |
| `drain_adversarial_tests` | corrupted, truncated, and extended snapshots; every single-byte frame mutation; 4,000 random JSON documents; replayed and reordered evidence; illegal transitions; oversized inputs |
| `drain_concurrency_tests` | admission racing closure, duplicate callbacks across eight threads, cancellation racing evacuation, reentrant hooks, repeated server start/stop |
| `drain_restart_tests` | incarnation and epoch monotonicity, evidence quarantine, conservative recovery, cancelled drains staying fenced across a restart, boot-marker corruption |
| `drain_transport_tests` | independent controller and agent processes over real TCP, hard process kill and restart with fresh-incarnation fencing, malformed byte streams, the CLI driven as separate processes |
| `drain_e2e_tests` | capacity and redundancy validation before removal, alternate-path loss, stale flow evidence, grace expiry with exceptions, retry exhaustion, cancellation and restoration, correlated sets, advisory obligations |
| `tests/downstream` | an independent CMake project consuming the installed package through `find_package` |

The whole suite runs plainly, with no timeouts of any kind: a hanging test is a
defect, not something to hide behind a watchdog.

## Performance

`drain_benchmark` measures **completed** work: fully drained targets (admission
closed, obligations evacuated and retired, removal validated, verification
passed) and completed persistence round trips. Median of three runs of the
Release build of this revision on the development machine:

| Workload | Completed | Elapsed | Throughput |
| --- | --- | --- | --- |
| 64 resources / 256 obligations | 64 / 64 drains | 0.0105 s | ~6,100 completed drains/s |
| 512 resources / 4,096 obligations | 512 / 512 drains | 0.917 s | ~558 completed drains/s |
| Snapshot round trips (2,000-entry payload) | 50 / 50 round trips | 0.36 s | ~139 completed round trips/s |

Both drain workloads are limited by the declared failure-domain concurrency of
one drain per domain, so they measure the full coordination path -- admission
closure, evacuation, retirement, removal validation, and verification -- rather
than raw bookkeeping. These numbers describe this runtime on one machine. They
are not a claim about fabric hardware.

## Layout

```
include/drain/     public API headers
src/               library implementation
tools/             drainctl, drainfabricd, drainagent
examples/          lifecycle, correlated-drain, and snapshot-inspection examples
benchmarks/        completed-work benchmark
tests/             seven test binaries plus an independent downstream consumer
docs/              architecture, semantics, concurrency audit, operations, proof surfaces
```

## Documentation

* `docs/architecture.md` — the boundary, the layers, and the engine pass.
* `docs/semantics.md` — identities, fences, obligations, evidence, the removal
  step, grace and exceptions, correlated drains, accounting closure.
* `docs/concurrency.md` — the ownership table, lock ordering, the lock and
  reentrancy audit, bounds, and the defects that audit produced.
* `docs/operations.md` — building, installing, the daemon, the agent, the CLI,
  the on-disk format, the recovery contract, and how to diagnose a stuck drain.
* `docs/proof-surfaces.md` — the explicit REAL / SYNTHETIC / UNSUPPORTED
  boundary.

## Requirements

A C++20 compiler and CMake 3.20 or newer. MSVC 19.40+ (Visual Studio 2022) is
the primary toolchain and is built with `/W4 /WX`; GCC and Clang are supported
by the same CMake project. AddressSanitizer is supported through
`-DDRAINFABRIC_SANITIZE=ON`. There are no third-party dependencies beyond the
platform's sockets library and the C++ standard library.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
