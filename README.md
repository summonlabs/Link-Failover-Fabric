# Link Failover Fabric

**Link Failover Fabric 1.0.0** is an authoritative failover authority runtime for individual
fabric links. It answers exactly one question, deterministically and truthfully:

> Given an authoritative link failure, current topology, alternate connectivity, policy,
> service obligations and exact generations — which replacement link or dependency may become
> authoritative **now**, what must be fenced first, and when must failover be refused, rolled
> back, revalidated, or declared indeterminate?

The repository is not complete unless that answer is deterministic and truthful under stale,
partial, contradictory, restarted, concurrent and adversarial conditions.

## Systems boundary

**Owned.** Deciding whether a failed or unusable link is no longer eligible; deciding which
explicitly supplied alternate link or dependency may replace it; and owning the lifecycle of
that failover authority, including fencing, rollback and post-restart revalidation.

**Not owned, and deliberately not absorbed.** Topology discovery, arbitrary end-to-end route
computation, bandwidth reservation, switch/NIC programming, traffic pacing, congestion
synthesis and hardware repair. Those responsibilities live behind explicit typed inputs and a
separate applier process. Adjacent runtimes integrate through typed evidence, references and
authority vectors — never by the engine reaching into them.

## Authority and generation model

Every decision is bound to a *generation-complete* authority vector. Matching identity is never
matching generation, so a comparison of names or sequence numbers is never sufficient.

| Field | Meaning |
| --- | --- |
| `subject` / `subject_generation` | the failed link and the exact generation it was observed at |
| `replacement` / `replacement_generation` | the alternate and the exact generation that may carry traffic |
| `topology_generation` + digest | the topology definition the decision was made against |
| `policy_generation` + digest | the policy the decision is legal under |
| `epoch` | the coordinator term |
| `coordinator` | the coordinator process incarnation (one per boot, never reused) |
| `evidence_publisher` / `publisher_incarnation` / `publisher_seq` | the exact observation that justified the decision |
| `attempt_seq` / `attempt_id` | the monotonic, deterministic attempt identity for the governed generation |
| `applier_incarnation` | the process that performed the effect, once one exists |
| `evidence_digest` | a content digest over the governing evidence |

An `AuthorityExpectation` lets a caller assert the fields it believes are still current.
Every mismatch is reported individually and field by field — never collapsed into "stale".

### Assertion ladder

Observation, eligibility, recommendation, authorization, acknowledgement and verified effect
are separate levels. A decision reports the **highest rung it actually reached**, and the reason
vocabulary makes the difference explicit (for example `AcknowledgementNotEffect`).

```
Observation < Eligibility < Recommendation < Authorization < Acknowledgement < VerifiedEffect
```

## Product-defining invariants

1. **Evidence is not authority.** A failed link being DOWN never authorises a replacement by
   itself; eligibility must be proven under current evidence and adjacent authority.
2. **Single winner.** At most one attempt per (fabric, subject, subject generation) may hold
   authority. Replayed, duplicate, stale and competing attempts cannot both become authoritative.
3. **Fail closed.** If failure evidence is UNKNOWN, stale, conflicted or below the policy
   confidence, no failover authority is manufactured. UNKNOWN never becomes affirmative.
4. **Indeterminate is not infeasible.** `PROVEN_INFEASIBLE` is emitted only with a certificate
   naming, for every supplied alternate, the disqualifying property that was observed under
   *current* evidence. One undecided alternate downgrades the answer to INDETERMINATE.
5. **Observations never age by publisher clock.** Freshness is computed from the coordinator's
   own monotonic receipt tick, epoch and incarnation. A publisher's wall clock is advisory
   metadata and is never used to order, age or justify a decision.
6. **Persistence is not liveness.** A restart preserves durable lineage and restores no live
   authority. Every pre-restart in-flight attempt is fenced as INTERRUPTED and must be
   revalidated.
7. **Acknowledgement is not verified effect.** With `require_verified_effect` (the default), an
   acknowledged but unverified application stops at `Acknowledgement` and reports
   `Outcome::Indeterminate`.
8. **Rollback is a new transition.** Restoring the original link is a new generation-bound
   transition with its own fence; it is never a silent resurrection of the old generation.
9. **Authority is revocable.** A policy or topology generation change fences every live grant,
   and a late acknowledgement for a fenced attempt is refused.
10. **Bounded everywhere.** Candidates, evidence tables, publishers, attempts, fences,
    idempotency entries, decisions, retained history, frame payloads and explanation reasons all
    have explicit bounds. Exhaustion is a deterministic refusal, never instability.
11. **Deterministic.** Canonical encodings, a total objective with a canonical tie-break, and no
    dependence on container, hash, insertion or discovery order. Two runs with the same inputs
    produce identical decision identities.

## Decision outcomes

| Outcome | Meaning |
| --- | --- |
| `Ok` | the requested assertion was established (for a grant: authorization) |
| `Refused` | conclusively not allowed under current evidence and policy |
| `Unknown` | evidence or effect state is not knowable now |
| `Stale` | a bound generation, epoch or incarnation is no longer current |
| `Conflict` | another authority, attempt or session owns this subject |
| `Indeterminate` | no total order or proof could decide the question |
| `Interrupted` | a pre-restart in-flight operation with unknown effect |
| `Invalid`, `Unsupported`, `NotFound`, `LimitExceeded`, `Corrupt`, `Exhausted`, `IoError`, `Closed`, `Unauthenticated` | explicit, non-conflated failures |

## Lifecycle

```
Evaluated ──► Authorized ──► Acknowledged ──► Verified ──► Committed
    │             │                │
    │             ├────────────────┴──────────► Interrupted   (restart)
    │             └───────────────────────────► Aborted       (applier refused, fence, revalidation)
    ├─────────────────────────────────────────► Refused / Indeterminate  (no authority created)
    └─────────────────────────────────────────► Superseded
RolledBack  ◄── an authorized attempt that was reverted as a new transition
```

A grant is written in two durable steps — an `Evaluated` intent record, then an `Authorized`
grant record — so that every crash boundary is an observable state:

| Crash point | Durable result | Post-restart observation |
| --- | --- | --- |
| before the intent record | nothing | no attempt, no authority |
| between intent and grant | `Evaluated` | `Interrupted` + `InterruptedBeforeCommit` |
| after the grant | `Authorized` | `Interrupted` + `InterruptedAfterCommit` |
| after an applier acknowledged | `Acknowledged` | `Interrupted` + `InterruptedAfterApply` |
| after verification, before completion | `Verified` | `Interrupted` + `InterruptedAfterApply` |

## Restart semantics

On open the store loads only what is legitimately durable — fabric definition, policy,
topology, committed lineage, fences, completions, rollbacks and the idempotency index — then:

1. advances the coordinator epoch and generates a fresh process incarnation;
2. writes a boot record and a pre-restart fence for every attempt that was in flight;
3. marks every non-terminal attempt `Interrupted`, preserving its original authority vector as
   history (it names the epoch and incarnation that minted it, which no longer exist);
4. refuses new failover for a generation with an unresolved interrupted attempt until an
   explicit revalidation resolves it.

**Dynamic state is never restored.** Observation receipts, evidence freshness, active leases,
in-flight authority and backend effects do not survive. After a restart, a failover request is
`Indeterminate` until fresh observations arrive; evidence published before the restart is gone
by design, and the runtime says so rather than pretending it is still current.

## Persistence

Two versioned, integrity-checked artefacts in one directory:

* `lineage.jrnl` — an append-only record journal with a 32-byte file header and 32-byte record
  headers. Each record carries a magic, format version, type, payload length, sequence, payload
  CRC-32C and header CRC-32C. Appends are flushed **and forced to the medium** before the call
  returns; the in-memory lineage is applied only after the append succeeds.
* `lineage.snap` — a transactionally replaced snapshot (write to `.tmp`, flush, atomic rename),
  plus a `Checkpoint` record in the fresh journal segment.

Recovery rules, in full:

* A record that is only **partially present at end-of-file** is a torn tail and is recovered by
  truncating back to the last complete record — but only when the caller asked for repair
  (`--strict-tail` / `repair_torn_tail = false` refuses instead, leaving the file untouched).
* A record that is **fully present but fails its checksum**, declares an unsupported version,
  names an unknown record type, sets a reserved bit, or regresses its sequence is **corruption**.
  Opening fails with `Outcome::Corrupt` and nothing is truncated, repaired or upgraded.
* Impossible declared lengths are refused before any allocation.
* A snapshot whose header, payload checksum, declared length, element counts or trailing bytes
  do not check out is refused and never truncated.
* Durable lineage belonging to a different fabric is refused with `Outcome::Conflict`.

## Transport

A bounded framed protocol over TCP: 20-byte header (magic `LFF1`, version, type, flags,
reserved, payload length, CRC-32C over the 16 header bytes that precede the CRC field followed
by the payload) and a payload of at most the negotiated maximum (256 KiB by default, 1 MiB hard
ceiling). Decoding is total and sticky: one bad frame ends the session permanently.

Every post-handshake request carries a session envelope — session id, coordinator epoch, client
incarnation, monotonic sequence. The server refuses:

* a request that does not belong to the session (`SessionMismatch`),
* a request that does not carry the session's incarnation (`IncarnationMismatch`),
* a request bound to an epoch that is not current (`EpochMismatch`),
* a replayed or regressed sequence (`SessionSequenceReplayed`).

One session can never act under another session's identity, boot or epoch.

### Trust boundary

The protocol provides framing integrity and a session authority model. **It performs no
authentication and no encryption, and it must not be exposed to an untrusted network.** CRC-32C
and SHA-256 are used for integrity and identity, not as security mechanisms. The applier is a
trusted peer on the same host; the runtime does not claim secure transport.

## Build

Requirements: CMake ≥ 3.20 and a C++20 compiler. There are **no third-party runtime
dependencies**; CRC-32C, SHA-256, canonical encoding and the socket layer are implemented in
this repository against the C++ standard library and the operating system only.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure     # or run build/lff-tests directly
```

Options: `LFF_BUILD_TOOLS`, `LFF_BUILD_TESTS`, `LFF_BUILD_EXAMPLES`, `LFF_BUILD_BENCHMARKS`,
`LFF_BUILD_SHARED`, `LFF_WARNINGS_AS_ERRORS` (all ON by default except `LFF_BUILD_SHARED`), and
`LFF_ENABLE_FAULT_INJECTION` (ON by default; see *Fault injection* below).

## Install and consume

```sh
cmake --install build --prefix /path/to/prefix
```

The package exports the namespace `lff::`, a `LinkFailoverFabricConfig.cmake`, a version file,
the public headers and the tools. An independent project consumes it with:

```cmake
find_package(LinkFailoverFabric CONFIG REQUIRED)
target_link_libraries(my-target PRIVATE lff::lff)
```

`consumer/` in this repository is exactly such a project. The `consumer` test suite installs the
package into a throwaway prefix, copies the project outside the source tree, configures, builds
and runs it against that prefix alone.

## Tools

| Tool | Role |
| --- | --- |
| `lff-fabricd` | coordinator daemon: owns one durable lineage directory, one epoch and one framed session service |
| `lff-worker` | external applier: claims directives, performs a real filesystem effect, reports the acknowledgement and then the observed verification |
| `lffctl` | operator and publisher client: publishes observations, requests decisions, reports applications and verifications, inspects state |

```sh
lff-fabricd --state=./state --fabric=demo --listen=127.0.0.1:7411 \
            --link=uplink-a:2:1000,uplink-b:5:4000 --ready-file=./ready.txt &
lff-worker --endpoint=127.0.0.1:7411 --effect-dir=./effects &
lffctl --endpoint=127.0.0.1:7411 publish-failure --link=uplink-a --generation=2 \
       --state=Down --confidence=1000 --publisher=monitor --observation-seq=1
lffctl --endpoint=127.0.0.1:7411 publish-replacement --link=uplink-b --generation=5 \
       --state=Up --confidence=1000 --capacity=4000 --publisher=monitor --observation-seq=2
lffctl --endpoint=127.0.0.1:7411 failover --link=uplink-a --generation=2 --candidates=uplink-b
lffctl --endpoint=127.0.0.1:7411 inspect --history=4
```

Every command prints deterministic `key=value` lines (`outcome`, `kind`, `assertion`,
`attempt_seq`, `attempt_id`, `decision_id`, `durable`, `lineage_seq`, `reason`, `candidate`).
Exit codes: `0` for `Outcome::Ok`, `1` for a well-formed negative result, `2` for a usage error,
`3` for a transport failure.

`examples/minimal_failover.cpp` runs a complete synthetic failover in one process.

### Fault injection

Compiled in unless `LFF_ENABLE_FAULT_INJECTION=OFF`, and inert unless `LFF_CRASH_POINT` names a
point. A hit terminates the process immediately — no destructors, no flush, no shutdown record.

| `LFF_CRASH_POINT` | Terminates |
| --- | --- |
| `before-commit` | before the intent record is appended |
| `after-commit` | after the grant is durable, before any response |
| `after-apply` | after an applier acknowledgement is durable |
| `before-completion` | in `lff-worker`, after the effect exists but before it is reported |
| `during-checkpoint` | between the snapshot write and the journal rotation |

## Testing

`build/lff-tests` runs every suite in one process; `--suite NAME`, `--filter TEXT` and `--list`
select cases. **No test uses a timeout parameter**, and a hanging test is treated as a defect to
diagnose rather than to mask. Property suites accept `LFF_PROPERTY_SEED` for reproduction.

| Suite | Cases | What it proves |
| --- | --- | --- |
| `unit` | 75 | identity grammar, checked arithmetic, canonical codec, selection trichotomy, engine semantics |
| `protocol` | 19 | framing, every truncated prefix, sticky failure, reserved bits, oversized claims, envelope authority |
| `persistence` | 16 | torn tails vs corruption, unsupported versions, unknown types, sequence regression, payload checksums |
| `integration` | 10 | loopback sessions, session identity, replayed sequences, prompt shutdown, oversized frames |
| `property` | 3 | invariants re-checked after every operation, determinism across identical runs, bounded history |
| `adversarial` | 14 | contradictory evidence, duplicate and late completions, exhaustion, malformed inputs, corruption |
| `concurrency` | 5 | barrier-released racing grants, concurrent observations, concurrent apply/verify, checkpoint races |
| `multiprocess` | 8 | real processes, real loopback sockets, hard kill at four boundaries, fresh-boot fencing |
| `restart` | 8 | lineage preserved, no authority restored, revalidation, snapshot compaction, epoch advance |
| `scale` | 3 | completed work at multiple sizes, bounded retained state, near-linear per-operation cost |
| `consumer` | 1 | install into a prefix, configure and build an independent project outside the source tree |

The selection algorithm is additionally differential-tested against a slow reference solver that
enumerates every ordering of the candidate set over 4000 seeded instances, and against instances
constructed specifically to defeat greedy "largest capacity", "first usable", "cheapest" and
"freshest" heuristics.

### Threat model of the tests

Every adversarial case is a *deterministic* input, not a probabilistic fuzz. Property and
differential corpora are seeded and the seed can be replayed. Concurrency tests release threads
from a barrier rather than sleeping, so the outcome is deterministic in kind (exactly one
winner) even though the interleaving is not.

## Evidence: REAL / SYNTHETIC / UNSUPPORTED

### REAL — actually exercised on this host

* **Multiprocess failover.** `lff-fabricd`, `lff-worker` and `lffctl` are independent operating
  system processes communicating over real loopback TCP sockets. The suite asserts on their exit
  codes, their captured output and the runtime's post-restart state.
* **Hard kill at meaningful boundaries.** `TerminateProcess` for the coordinator and an immediate
  `_exit(97)` for the worker at four distinct points: before the durable commit, after the
  durable commit but before acknowledgement, after the effect exists but before completion, and
  after an acknowledgement but before verification. The suite verifies the resulting state is
  conservative and fenced.
* **Concurrent processes.** Six independent `lffctl` processes race for one governed link
  generation; exactly one is granted and the other five are refused with `Conflict`.
* **Durable persistence.** Real files, `fflush` + `_commit`, atomic rename, and reopening the
  lineage from disk after real process termination.
* **Address-independent builds.** Release and Debug builds, an installed package, and an
  independent downstream project built and executed against the installed prefix.

### SYNTHETIC — deterministic fixtures, clearly labelled

* The fabric, its links, their capacities and generations.
* Every failure and replacement observation, including contradictory and low-confidence ones.
* The applier's effect: `lff-worker` writes a record file into a directory it owns. This is a
  real filesystem effect, but it is **not** a switch, NIC, RDMA, DPU or switch-ASIC
  reconfiguration, and no such hardware is claimed.
* All service-obligation and adjacency-authority inputs.

### UNSUPPORTED on this host

* **AddressSanitizer and UndefinedBehaviorSanitizer.** The MSVC sanitizer runtime is not
  installed. Concretely missing: `clang_rt.asan_static_runtime_thunk-x86_64.lib` and
  `clang_rt.asan_dynamic-x86_64.lib` for x64 (only the i386 runtimes are present), and
  `LIBVCASAN.lib` for x86 linking. `/fsanitize=address` fails at link time on both
  architectures. **No sanitizer coverage is claimed.** The strongest available substitutes were
  used instead: MSVC iterator debugging and `/RTC1` runtime checks in Debug,
  `/W4 /WX /permissive-` in both configurations, and clang-tidy static analysis.
* **Physical fabric behaviour.** No switch, NIC, RDMA, RoCE, InfiniBand, SmartNIC, DPU, NVLink,
  multi-node or switch-ASIC behaviour was exercised, and none is claimed.
* **Authenticated or encrypted transport.** Intentionally out of scope; see the trust boundary.

## Performance

`lff-bench [scale]` reports completed work only. Measurements from this host (Release, MSVC
19.44, x64; every durable record is forced to the medium):

| Measurement | scale 1 | scale 4 | Notes |
| --- | --- | --- | --- |
| durable journal appends | 949 /s | 962 /s | one `fflush` + `_commit` per record |
| accepted observations | 976 734 /s | 347 855 /s | in-memory, bounded table |
| completed failover decisions | 169 321 /s | 72 797 /s | in-memory once the generation budget is spent |
| answered service requests | 58 899 /s | 56 470 /s | four concurrent loopback sessions |

These are single representative runs on a shared developer host and vary with machine load; the
scale-4 column was measured while a Debug build was running alongside it. **Scaling claims rest
on the `scale` suite, not on this table**: it measures 16.9 µs per completed operation at 2 000
operations and 16.7 µs at 20 000 — a per-operation ratio of **0.99** across a 10× increase in
workload, with retained state bounded and flat.

Grouped by cost class: an **in-memory** decision costs tens of microseconds; a **durable**
decision costs two forced writes to the medium, which is what the ~1 000/s journal figure
measures; and a **compacted** run additionally pays a snapshot every
`--snapshot-every` records.

`lff-bench` measures *completed* work: decisions that were made and committed, observations that
were accepted, records that reached the medium, and requests that were answered. The `scale`
suite additionally asserts that retained state stays bounded and that per-operation cost does not
grow with history (a 10× workload increase must not cost more than 4× per operation; measured
ratio 1.76, dominated by snapshot compaction).

Retained state is bounded by configuration: attempts, fences, idempotency entries, decision
history, tracked publishers and evidence entries all have explicit ceilings, and the journal is
compacted by snapshot. No plan, explanation, candidate list or history grows without a bound.

## Limits and honest caveats

* **Single host, single coordinator.** There is no leader election, no consensus and no
  multi-node coordination. One durable lineage directory is owned by one coordinator process.
* **No authentication or encryption.** See the trust boundary above. Run it on loopback or a
  trusted segment.
* **Evidence is not durable by design.** After a restart, failover requires fresh observations.
  This is a deliberate consequence of *persistence is not liveness*, not a defect, but it means a
  restart always costs at least one fresh observation round.
* **A policy or topology change fences every live grant**, including unrelated ones. This is
  fail-closed and deliberate; it is coarser than per-subject invalidation.
* **Frame payloads are capped at 256 KiB by default** (1 MiB hard ceiling), which bounds the
  candidate list a single request may carry.
* **A blocked `send` to a peer that stops reading is not forcibly interrupted.** The session
  loop polls for readability so shutdown is prompt, but a write already in progress drains or
  fails on its own. With a trusted peer and a 256 KiB frame cap this window is small; it is
  documented rather than hidden. See `docs/CONCURRENCY_AUDIT.md`.
* **Candidate sets are bounded at 256** and topology at 4096 links, by policy and by validation.
* **The selection objective is a total lexicographic order**, not a weighted cost model: there is
  no notion of price, SLA or traffic matrix beyond declared capacity, cost and adjacency
  authorization.
* **No route computation.** The runtime selects a single-hop alternate; it does not compute
  end-to-end paths, reserve bandwidth or pace traffic.
* **Windows-first validation.** The code is portable C++20 with a small POSIX branch in the
  socket and filesystem helpers, but only the Windows/MSVC toolchain was built and exercised
  here. The POSIX paths are compiled-by-inspection only and are marked accordingly.
* **Version 1.0.0 formats are frozen.** Raising `kJournalFormatVersion`,
  `kSnapshotFormatVersion`, `kWireProtocolVersion` or `kIdentityFormatVersion` is a breaking
  change; older artefacts are refused with `Outcome::Unsupported` rather than reinterpreted.

## Repository layout

```
include/lff/      public headers (the installed surface)
src/              runtime implementation
src/detail/       internal helpers: sockets, filesystem durability, fault injection, tool support
tools/            lff-fabricd, lff-worker, lffctl
tests/            the suites listed above
benchmarks/       completed-work benchmark
examples/         minimal single-process failover
consumer/         an independent downstream project used by the install proof
docs/             the concurrency and ownership audit
scripts/          verification entry points
```

## Verification

`scripts/verify.ps1` (run from any directory) configures and builds Release and Debug, runs
every suite, runs the benchmark, and performs the install plus downstream-consumer check. It
contains no absolute paths and works from a fresh clone.

## License
Apache License 2.0. Copyright 2026 Summon Software Labs.
