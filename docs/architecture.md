# Drain Fabric architecture

## System boundary

Drain Fabric owns the controlled transition of a network resource from serving
to unavailable while preserving explicit active obligations.

It implements exactly four things:

1. **Admission closure.** A fence that stops new obligations from binding to a
   target, keyed by the target and stamped with the generation that closed it.
2. **Obligation accounting.** An explicit, bounded obligation model, an evidence
   ledger, and an authoritative snapshot that can always answer "what still
   depends on this resource".
3. **Readiness proof.** Capacity, redundancy, diversity, and connectivity
   validation before each removal step, plus a final verification that only
   accepts a fresh, generation-matched observation.
4. **Final drain authority.** A lifecycle that refuses to declare a resource
   drained while a protected obligation depends on it, and refuses to accept
   stale evidence, stale authority, or a foreign generation.

It deliberately does **not**:

* perform the maintenance that motivated the drain;
* reroute, migrate, or release an obligation (it *requests* those from adjacent
  runtimes through `EvacuationSink`, and never pretends to own them);
* recompute routing policy;
* decide upgrade versions;
* own the topology's policy semantics beyond the commitments it is given.

## Layers

```
 tools/          drainctl            drainfabricd            drainagent
                  CLI                 controller daemon       node agent
                    \                     |                      /
                     \                    |                     /
 server.cpp  ------------------- ControllerServer -----------------
             |  framed TCP        |  EvacuationSink implementation
             |  WireClient        |  session authorities
             v                    v
 engine.cpp  ---------------- DrainEngine ----------------
             |  lifecycle        |  admission fence   |  decisions
             |  obligations      |  evidence ledger   |  authority line
             |  drain sets       |  audit             |  explanations
             v
 topology.cpp  policy.cpp  lifecycle.cpp  obligation.cpp  authority.cpp
 evidence.cpp  admission.cpp  decision.cpp  explain.cpp  drain_set.cpp
             v
 identity.hpp  result.hpp  clock.hpp  checked.hpp  json.hpp  digest.hpp  crc32c.hpp
 persistence.cpp  runtime.cpp     durable, integrity-checked state and boot line
```

Every arrow points downwards only. The engine never calls the CLI, the tools
never mutate engine state except through the public API, and the transport layer
carries the same operations a local caller would use.

## Core objects

| Object | Identity | Meaning |
| --- | --- | --- |
| `DrainTarget` | kind + resource id + failure domain | what can be drained |
| `DrainRecord` | `DrainId` + `Generation` | one drain of one target |
| `DrainSet` | `DrainSetId` | a correlated batch with deterministic order |
| `Obligation` | `ObligationId` + `Revision` | a dependency carried by resources |
| `Evidence` | `EvidenceSeq` + `EvidenceKey` | one observation, fenced by incarnation and epoch |
| `AuthorityToken` | `AuthorityId` + `Epoch` + `Generation` + `IncarnationId` | who may mutate, and under which line |
| `Decision` | — | an inspectable record of one applied or refused action |

All of the numeric identities are distinct instantiations of the same template,
so a generation cannot be passed where an attempt is expected, and a
`CapacityUnits` cannot be added to a `MemberCount`.

## The engine pass

`DrainEngine::advance(now)` performs, for every non-settled drain in id order,
**at most one lifecycle transition**, plus, when the drain is blocked, a
revalidation pass that resumes it and immediately performs the transition it was
waiting for. Revalidation is not a lifecycle step; without the fusion a restart
would cost one scheduling pass per recovery.

Requests for adjacent runtimes are collected in a `PendingActions` batch under
the engine mutex and dispatched **after** the mutex is released. A hook may
therefore call back into the engine, which is exactly what the tests exercise.

## Lifecycle

```
Requested -> Validating -> AdmissionClosed -> Evacuating -> Quiescing -> Verifying -> Drained
                                |               |             |            |
                                +---------------+-------------+------------+--> Blocked
                                |               |             |            |
                                v               v             v            v
                             Cancelled <------ Failed ------> Restoring ----+
```

* `Evacuating` and `Quiescing` are distinct because they are driven by
  different adjacent-runtime operations: reroute/migration versus release and
  quiescence confirmation.
* `Blocked` remembers the state it is holding, so it can be resumed, explained,
  cancelled, or failed.
* `Restoring` reopens admission under a **fresh** generation and settles back
  into `Cancelled`: the drain did not happen and the target is in service again.
* The removal step happens at the `Quiescing -> Verifying` transition, after
  capacity and redundancy have been validated.

## Determinism

Given the same inputs and the same clock readings, the engine produces
byte-identical decisions, explanations, and snapshots. Nothing in the decision
path consults wall-clock time (only the injected `Clock`), unordered
containers, pointer values, or locale. The property tests assert this by running
the same randomized scenario twice and comparing the rendered transcripts.

## Where the state lives

* **In-memory authoritative state**: topology, obligations, fence, evidence,
  authority, drains, sets, exceptions, decisions.
* **Durable state**: a versioned, CRC-checked snapshot plus a boot marker that
  records the incarnation and epoch of the last start. See
  `docs/operations.md` for the recovery contract.
