# Drain Fabric semantics

## Identities and fencing

Every domain object carries an identity whose C++ type is unique to its kind:

* `ResourceId`, `DomainId`, `HolderId`, `GroupId`, `ServiceId`, `NodeId` —
  validated slugs (non-empty, at most 128 characters from `[A-Za-z0-9_.:-]`).
* `DrainId`, `DrainSetId`, `ObligationId`, `Generation`, `Epoch`,
  `IncarnationId`, `AuthorityId`, `EvidenceSeq`, `Revision`, `Attempt`,
  `RequestNonce` — distinct numeric types that cannot be interchanged.
* `CapacityUnits`, `MemberCount`, `ByteCount` — distinct quantity types.

Monotonic counters are advanced with checked arithmetic and refuse to wrap.

### The four fences

| Fence | Advanced by | Rejects |
| --- | --- | --- |
| Incarnation | every process start | tokens, evidence, and reports from a previous process |
| Epoch | every process start | evidence and tokens from a previous authority line |
| Generation | closing/reopening admission for a target | binds, reports, and evidence from a superseded boundary |
| Revision | every obligation state change | replayed or reordered callbacks |

A rejected mutation never partially applies: the fence is checked before the
first write.

## Admission boundary

`AdmissionFence::close` advances the target's generation and records the drain
that closed it. `AdmissionFence::reopen` advances the generation again and
records the drain that reopened it. A caller that wants to bind a new obligation
presents the generation it believes is current:

* fence never used, presented generation 0 → accepted;
* fence open at the presented generation → accepted;
* fence open at a different generation → `StaleGeneration`;
* fence closed at the presented generation → `AdmissionClosed`;
* fence closed at a different generation → `StaleGeneration`.

Both refusals keep the invariant "no new obligation binds after admission is
authoritatively closed". The distinction only tells the caller whether it needs
to re-read state.

One target has exactly one owner at a time. A second drain request for a target
whose previous drain is not fully restored is refused with `DuplicateIdentity`;
only a drain that reached `Cancelled` **and** was restored may be replaced.

## Obligations

An obligation is a dependency: an active flow, a reservation, a workload network
contract, a path-diversity commitment, a capacity guarantee, or a supplied
dependency. Each carries a protection class:

* **Protected** — blocks drain completion until it is retired.
* **Advisory** — recorded, reported, and counted, but never blocking.

Obligation states: `Admitted -> Evacuating -> Quiescing -> Released -> Retired`,
with `Failed` as a reported failure and a legal path back to `Admitted` from
`Retired`, `Released`, `Quiescing`, and `Failed`.

The path back to `Admitted` is how an **obligation reappearance** is modelled: an
adjacent runtime re-binds a flow that Drain Fabric had already accounted for as
gone. When that happens the audit reports the inconsistency and any drain that
already completed for the target is visible as a violation — the runtime never
hides it.

A report that repeats the current state with a **newer** evidence sequence is a
re-observation: the stored proof is replaced and the revision advances, so older
replays stay fenced.

## Evidence

Evidence is the only currency that can prove a drain step. Every record is bound
to the incarnation that produced it, the authority epoch, the drain generation,
and the instant it was observed.

* Sequences are strictly increasing per key: replays and rewinds are rejected.
* An observation beyond the tolerated clock skew is rejected; inside the skew it
  is accepted (adjacent runtimes do not share a clock, and the tolerance is
  explicit policy: `evidence_clock_skew`, 5 seconds by default).
* Recovered records are **quarantined**: deserialisation never makes them
  current. Only the live incarnation may re-attest its own record.
* Freshness is measured from the observation instant; stale evidence is reported
  as a blocker, never silently ignored.
* Evidence observed before the admission boundary cannot prove post-boundary
  quiescence.

The one exception is deliberate and narrow: when a restart quarantines a
**removal** observation, the engine may take a *new* observation if live
authoritative state still proves the removal — the fence is closed at the
matching generation, the resource is `Unavailable` (i.e. this runtime took it
out of service, not an adjacent failure report), and zero protected obligations
remain. That is a fresh observation, not reuse of recovered evidence, and it can
be switched off with `reobserve_removal_evidence=false`.

## Removal ownership

The removal step is only performed when the drain is at `Quiescing` with nothing
left to retire. Immediately before the resource leaves service the engine
validates:

1. the resource exists and is in service;
2. every capacity pool that contains it still meets its commitment (with the
   policy headroom ratio applied, in integer arithmetic);
3. every diversity group that a declared path through it belongs to still has
   the required number of available paths;
4. every protected route still has a route between its endpoints.

A drain may only claim a removal **it performed**: the resource must be
`Unavailable` and the admission fence must be closed by that drain at that
generation. A resource that was reported `Failed` by an adjacent runtime is not
a drained resource, and a drain that finds it that way blocks with
`TopologyInconsistent` rather than claiming someone else's removal.

## Grace, deadlines, and exceptions

Policy deadlines never fail or complete a drain. They produce blockers:

* `ObligationGraceExpired` — the guaranteed window elapsed without a release.
* `EvacuationAttemptsExhausted` — the bounded automatic retry budget is spent.

Both are cleared by `grant_exception`, which extends the window for a specific
obligation (or every outstanding one) and resets the retry budget. Exceptions are
bounded by `max_exception_extensions` and carry their own authority and
sequence.

There is no wall-clock timeout anywhere in the lifecycle. A drain that cannot
progress says exactly why and waits for an operator or an adjacent runtime.

## Correlated drains

A `DrainSet` is ordered by `target_order_less`: failure domain first, then kind
rank, then resource identity. Caller order is irrelevant.

At request time the whole set is checked against the commitments **as if it had
already been applied**. A set that cannot be removed without breaking a
commitment is refused before any of its members closes admission.

At scheduling time a member may only close admission if:

* its failure domain hosts fewer than `max_concurrent_drains_per_domain`
  fence-holding drains;
* it is the lowest-numbered drain in its domain that has not yet closed
  admission (deterministic admission order, which is what makes correlated
  admission deadlock free);
* when `serialize_same_domain_in_set` is set, every earlier member of its set in
  the same domain has settled.

## Accounting closure

`DrainEngine::audit` recomputes the following from the authoritative snapshot and
reports every violation it finds:

* every `Drained` drain has zero outstanding protected dependents, a non-zero
  completion digest, a recorded zero remaining-protected count, a closed fence,
  and a resource that is out of service;
* every non-settled drain that is past admission closure holds a closed fence;
* every `Cancelled` drain that closed admission still holds the fence, and one
  that was restored has advanced the generation and no longer owns it;
* no obligation was admitted at a generation at or after the closure of its
  target;
* every closed fence is owned by a known drain;
* the drain state counts add up to the drain total.
