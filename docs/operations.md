# Drain Fabric operations

## Building

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Options:

| Option | Default | Effect |
| --- | --- | --- |
| `DRAINFABRIC_BUILD_TESTS` | ON | build the seven test binaries |
| `DRAINFABRIC_BUILD_TOOLS` | ON | build `drainctl`, `drainfabricd`, `drainagent` |
| `DRAINFABRIC_BUILD_EXAMPLES` | ON | build the examples |
| `DRAINFABRIC_BUILD_BENCHMARKS` | OFF | build `drain_benchmark` |
| `DRAINFABRIC_WERROR` | ON | treat first-party warnings as errors |
| `DRAINFABRIC_SANITIZE` | OFF | AddressSanitizer + UndefinedBehaviorSanitizer where supported |
| `DRAINFABRIC_SHARED` | OFF | build the library as a shared object |

Installing and consuming:

```sh
cmake --install build/release --prefix /some/prefix
cmake -S tests/downstream -B build/downstream -DCMAKE_PREFIX_PATH=/some/prefix
cmake --build build/downstream && ./build/downstream/downstream_consumer
```

## The controller daemon

```sh
drainfabricd --state /var/lib/drain/state.drainlog \
             --listen 127.0.0.1:9100 \
             --topology /etc/drain/topology.json \
             --policy /etc/drain/policy.json \
             --ready-file /run/drainfabricd.ready \
             --exit-after-requests 0
```

* `--listen HOST:PORT` — port 0 asks the platform for an ephemeral port; the
  chosen port is written to the ready file as `port=N`.
* `--exit-after-requests N` — a deterministic termination condition. The
  handshake is not counted; operator requests are.
* `--workers N` — run N polling workers instead of one loop.
* The daemon stops on `SIGINT`/`SIGTERM`, on a `Shutdown` frame, or when the
  request budget is reached; in every case it revokes authority and writes a
  final snapshot.

## The node agent

```sh
drainagent --connect 127.0.0.1:9100 --node node-7 \
           --obligations /etc/drain/node-7-obligations.json \
           --ready-file /run/drainagent.ready --exit-after 0
```

The agent registers the obligations it holds, then answers evacuation requests by
recording release evidence and reporting the obligation state back. `--fail`
reports evacuation failure instead of releasing; `--ignore` acknowledges without
releasing. Both are used by the failure-injection tests.

## The operator CLI

```sh
drainctl --state ./state.drainlog init
drainctl --state ./state.drainlog load-topology --file topology.json
drainctl --state ./state.drainlog admit --holder workload-7 --deps resource:leaf-1@pod-a
drainctl --state ./state.drainlog request --target resource:leaf-1@pod-a --reason maintenance
drainctl --state ./state.drainlog advance --steps 8
drainctl --state ./state.drainlog status
drainctl --state ./state.drainlog explain --drain 1
drainctl --state ./state.drainlog obligations --target resource:leaf-1@pod-a
drainctl --state ./state.drainlog cancel --drain 1 --reason "abort"
drainctl --state ./state.drainlog restore --drain 1
drainctl --state ./state.drainlog audit
drainctl --state ./state.drainlog verify --file ./state.drainlog
drainctl --state ./state.drainlog snapshot --out ./state.json
```

Exit codes: 0 success, 1 usage error, 2 operation rejected, 3 not found,
4 stale fence, 5 integrity failure.

Each invocation is its own process and therefore its own incarnation. That is
safe because:

* the drain request is durable: a drain that had not yet closed admission
  continues from where it was, because its authority was accepted by the
  incarnation that recorded it;
* evidence produced by a finished incarnation is quarantined, which is why
  `release` performs its own advance pass in the same invocation
  (`--advance N`, default 6);
* a removal observation is re-derived from live state when the drain is resumed,
  so a drain that only needs its removal step confirmed completes normally.

## Durable state

```
 0   magic         8   "DRNFAB\x00\x01"
 8   format        u16 snapshot format version (currently 1)
10   header flags  u16 must be zero
12   header CRC    u32 CRC-32C over bytes [0, 12)
16   sequence      u64 strictly increasing per writer
24   incarnation   u64 incarnation that wrote the file
32   payload len   u64
40   payload CRC   u32 CRC-32C over the payload
44   reserved      u32 0xffffffff
48   payload           UTF-8 JSON
     file CRC      u32 CRC-32C over bytes [0, 48 + payload len)
```

Alongside the snapshot, `<state>.boot` records the incarnation and epoch of the
last start, so a hard kill that happens before any snapshot is written still
advances both.

Writing is atomic: the new snapshot goes to `<state>.tmp`, is flushed to stable
storage, the previous file is rotated to `<state>.bak`, and only then is the new
file moved into place. Persistence growth is bounded: one live file, one backup,
one temporary file.

## Recovery contract

1. Read the boot marker. A malformed or checksum-mismatched marker is a hard
   `PersistenceCorrupt` failure; the runtime refuses to guess.
2. Choose the new incarnation and epoch as `1 + max(boot marker, snapshot,
   configured boot id)`. Both strictly increase, even after a hard kill.
3. Load the snapshot, verifying every checksum and length. If the live file is
   rejected, fall back to the backup exactly once and report that it was used.
4. Quarantine every evidence record and revoke every authority token.
5. Conservatively block every drain that was in a transient state
   (`Validating`, `AdmissionClosed`, `Evacuating`, `Quiescing`, `Verifying`,
   `Restoring`) with `RestartRecoveryPending`, remembering the state it came
   from.
6. Install the new authority line and revoke everything that came before.

A drain that was already `Drained` stays drained and remains auditable: the
completion digest, the recorded protected-dependency count, the closed fence,
and the out-of-service resource are all restored.

## Diagnosing a stuck drain

`drainctl explain --drain N` prints the state, the resumable state, the blocker
set, the dependents, the outstanding protected obligations, and the decision
history. Each blocker names the exact obligation, path, pool, or evidence key
that is holding the drain, and each decision records the inputs, the evidence
consulted, the governing policy revision and fingerprint, the generation and
authority under which it was taken, and the alternatives that were rejected.

The usual causes and their remedies:

| Blocker | Meaning | Remedy |
| --- | --- | --- |
| `protected-obligation-active` | something still depends on the target | wait for the adjacent runtime, or `drainctl release` |
| `release-evidence-missing` / `release-evidence-stale` | the release was not proven, or the proof aged out | re-report the release with a fresh observation |
| `obligation-grace-expired` | the guaranteed window elapsed | `grant_exception` or release the obligation |
| `evacuation-attempts-exhausted` | the automatic retry budget is spent | `grant_exception`, which resets it |
| `capacity-insufficient` / `path-diversity-violation` / `alternate-path-lost` | the removal would break a commitment | restore capacity or redundancy, or drain a different target |
| `domain-limit-reached` / `set-predecessor-pending` | correlated admission ordering | wait, or drain fewer targets per domain |
| `authority-invalid` | the live incarnation revoked the requesting token | re-issue authority and re-request the drain |
| `restart-recovery-pending` | recovered state needs revalidation | one more advance, or `resume` |
| `topology-inconsistent` | the target left service outside this drain | investigate, then cancel and restore |

## Benchmarking

```sh
./build/release/benchmarks/drain_benchmark 64 256
```

The benchmark counts **completed** drain lifecycles and **completed** snapshot
round trips, never submission or enqueue latency.
