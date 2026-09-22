# Proof surfaces: REAL, SYNTHETIC, UNSUPPORTED

This document is the honest boundary of what the repository has demonstrated.

## REAL — executed on this repository's CI-equivalent runs

| Claim | How it is proven |
| --- | --- |
| Controller/agent coordination over real framed TCP | `drain_transport_tests` starts `drainfabricd` and `drainagent` as independent OS processes, connects over loopback sockets, and completes a drain end to end |
| Process kill and fresh-incarnation fencing | The same suite hard-kills a daemon with `TerminateProcess`/`SIGKILL` (no graceful shutdown, no final save), restarts it against the same state path, and asserts that the incarnation and epoch advanced and that the previous session's authority token is refused |
| Framing integrity | Every byte of a valid frame is mutated in turn and the decoder is required to reject each mutation; truncation, appending, and oversized declarations are rejected too |
| Durable state integrity | Snapshots are written with a header CRC, a payload CRC, and a file CRC; corrupted, truncated, and extended files are rejected, and recovery falls back to the previous good snapshot |
| Restart conservatism | Recovered evidence is quarantined and cannot be re-attested by the new incarnation; transient drain states recover blocked and must revalidate |
| Concurrency | Admission races closure across four threads; duplicate callbacks race across eight; cancellation races evacuation; the reentrant sink calls back into the engine from two threads; server workers start and stop repeatedly |
| Operator CLI over durable state | `drainctl` is driven as a separate process for init, topology load, request, advance, status, audit, explain, obligations, and snapshot verification, each in its own incarnation |
| Packaging | The project installs to a prefix and an independent CMake project consumes it through `find_package(DrainFabric 1.0 REQUIRED)` and runs |
| Warning-free build | Release and Debug both build with MSVC `/W4 /WX`; the first-party warning count is zero |

## SYNTHETIC — modelled, not real hardware

| Area | What is modelled | What is not claimed |
| --- | --- | --- |
| Adjacent runtimes | Test sinks release obligations and record evidence the way a reroute or migration controller would | No real reroute, migration, or release engine exists in this repository |
| Topology | Resources, declared paths, diversity groups, capacity pools, and protected routes | Not a routing protocol, not a live topology discovery service, not vendor hardware |
| Capacity and redundancy | Integer capacity ledgers and declared path memberships | Not measured link capacity, not a hardware redundancy model |
| Clock skew | A bounded tolerance for peer observations | Not a clock-synchronisation implementation |
| Admission fence at adjacent runtimes | `AdmissionReopen` requests are sent and acknowledged | The adjacent runtime's own admission enforcement is out of scope |

## UNSUPPORTED — no claim is made

* No RDMA, NVLink, InfiniBand, or multi-GPU behaviour of any kind.
* No switch-vendor, firmware, or ASIC behaviour.
* No multi-node cluster consensus, leader election, or replicated log. The
  controller is a single process with durable local state; the transport is
  loopback TCP in the tests.
* No performance claim against real fabric hardware. The benchmark measures the
  runtime's own completed work on the build machine and nothing else.
* No claim that every possible reordering of every possible distributed schedule
  is safe; the property tests cover seeded randomized schedules, restart points,
  and the specific adversarial sequences enumerated in `docs/semantics.md`.
