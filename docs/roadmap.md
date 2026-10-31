# Implementation Roadmap

Each phase has a **done** definition that is falsifiable: a test that fails without the
feature, plus an invariant stated in the docs. A phase is not done because the code exists.

---

## 0. Dependency correction to the proposed phase order

The proposed order is 1 bitmap → 2 generations → 3 consumers → 4 history → 5 persistence
→ 6 simulator. That order has two inversions:

- Phase 4 (historical bitmap *persistence*, comparison *after restart*) depends on Phase 5's
  serialization and CRC infrastructure. Doing 4 before 5 means writing a throwaway
  serializer.
- Phase 3 (consumers) needs the history *lookup* interface (union since cursor) which is
  Phase 4's semantics. It can be done with in-memory deltas, but the interface must be
  defined by Phase 4's model or it will be redesigned.

**Recommended order:**

```
0 research  ->  1 bitmap  ->  2 generations  ->  3 history semantics (in-memory)
            ->  4 consumers  ->  5 persistence + recovery
            ->  6 simulator + CLI + crash matrix  ->  7 dm-era backend  ->  8 hardening
```

Mapping to the brief: old Phases 4 and 5 swap places and split; the rest keep their
content. Nothing is dropped.

---

## Phase 0 — Research and architecture validation

Deliverables: this document set, plus a measurement report `docs/dm-era-report.md`.

Experiments, each producing a number or a fact in the report:

| # | Experiment | Record |
|---|---|---|
| 0.1 | Create an `era` target at 64 KiB, 1 MiB, 4 MiB block size on a 16 GiB loop device. | create/checkpoint/dump wall time; metadata size growth per era (bytes) |
| 0.2 | Checkpoint 256 times; measure metadata device size after each 32. | growth curve vs. region count; confirm/deny unbounded growth |
| 0.3 | Take a metadata snapshot while I/O is flowing; `era_dump` it. | dump duration at 1 TiB-equivalent bitmap; is the result consistent? |
| 0.4 | Determine whether any in-kernel or ioctl path returns the era write set without an offline dump. | yes/no + evidence |
| 0.5 | Power-cut simulation: `echo 1 > /proc/sys/kernel/sysrq`-style reboot under load, or `dm-flakey` error injection on the metadata device. | after reboot, are the last N checkpoints present? define N |
| 0.6 | Measure `dmsetup suspend` + snapshot create + `resume` latency on a busy ext4 volume. | suspend duration p50/p99 — the number that decides operability |
| 0.7 | Confirm `era_invalidate` semantics for forcing a range dirty. | exact behaviour + cost |
| 0.8 | Check behaviour when the metadata device is a cached SSD vs. a `no-write-cache` device. | whether `durable_before_ack` can ever be true |
| 0.9 | Confirm discard handling. | does a discard mark the block dirty? |

**Done when:** every row has an answer with evidence, and R1–R4 in
[risks.md](risks.md) have either a mitigation or a decision (Q1).

---

## Phase 1 — DirtyBitmap library

Scope: `types.h`, `bitmap.h`, `src/core/bitmap.cpp`, CMake, GoogleTest, CI with
ASan/UBSan.

Interfaces: as in [bitmap-history.md](bitmap-history.md) §1.

Acceptance tests (all must fail before the implementation and pass after):

| Test | Assertion |
|---|---|
| `MarkUnalignedCrossBoundary` | 1 MiB device, write at 60000 len 10000 ⇒ regions {0,1} |
| `MarkZeroLength` | `Ok`, no bits set |
| `MarkAtEndOfDevice` | offset = capacity-1, len 1 ⇒ last region only |
| `MarkPastEnd` | `OutOfRange`, no bits set (all-or-nothing) |
| `MarkOverflow` | offset = 2^64-8, len = 16 ⇒ `OutOfRange`, no UB (UBSan clean) |
| `TailBitsNeverSet` | device 1 MiB + 1 byte ⇒ regionCount 17; `countDirty()` ≤ 17; words[0] high bits zero |
| `DuplicateMarkIdempotent` | mark same region 1000× ⇒ count 1 |
| `FullDeviceWrite` | count == regionCount, ranges == 1 covering the device |
| `RangeCoalescing` | {10,11,12,13,20,21,50} ⇒ 3 ranges with exact byte offsets |
| `BitwiseTailMask` | `andNot` on uneven lengths never sets bits past the tail |
| `CompareDifferentGranularity` | returns `InvalidState` |
| `CompareDifferentRegionCount` | returns `InvalidState` |
| `ConcurrentMarksDifferent` | 8 threads × 100 k marks; union exact |
| `ConcurrentMarksSame` | 8 threads marking region 0; count == 1; TSan clean |
| `RandomizedOracle` | 1 M random writes vs. single-threaded byte-range oracle over the whole device |

**Done when:** suite green under ASan+UBSan+TSan, no warnings at
`-Wall -Wextra -Wpedantic -Wconversion`.

---

## Phase 2 — Generation lifecycle (in-memory)

Scope: `DeviceContext`, `ActiveBitmap` with atomics, seal protocol, sealed-bitmap
publication.

Acceptance (straight from the brief's Phase 2 test, made precise):

```
1. mark {10, 20}                       -> D_active == {10,20}
2. seal()                              -> D_1 sealed, active gen = 2 (empty)
3. mark {30, 40}
4. assert sealed(1) == {10,20}   AND   active contains {30,40}
5. simulate backup failure at boundary 1 (no commit)
6. assert history still has D_1 and D_2's accumulated bits
7. re-attempt: seal -> D_2 == {30,40}; union(1..2) == {10,20,30,40}
8. assert nothing lost
```

Plus:

| Test | Assertion |
|---|---|
| `SealWaitsForInFlightMarks` | a mark started before seal lands in the old generation, exactly once |
| `NoMarkLostAtSeal` | 10 k seals under 8 writer threads; union of sealed dels == oracle |
| `SealedIsImmutable` | the published bitmap is `const`; attempting to mutate is a compile error |
| `SealIsIdempotent` | sealing the same generation twice returns `Ok` the second time |
| `PeriodicSealBound` | dirty-region threshold triggers a seal with a delta below the bound |

**Done when:** "no mark may be lost or duplicated across a seal" is demonstrated by
randomized testing, not by argument alone.

---

## Phase 3 — Historical bitmap semantics (in-memory)

Scope: `HistoryStore` (in-memory), `RangeQueryEngine`, union/diff, coalescing, cumulative
checkpoints at `K`.

Acceptance:

```
A = {10,20,30}, B = {20,30,40,50}
and(A,B)              == {20,30}
andNot(A,B)           == {10}
andNot(B,A)           == {40,50}
or(A,B)               == {10,20,30,40,50}
```

Plus property tests from [bitmap-history.md](bitmap-history.md) §4:
`union(a,c) == union(a,b) | union(b,c)`, and `union(a,b) == cum_b AND NOT cum_a` with cums
rebuilt from deltas.

Plus: `RebuildCumFromDeltas` — delete every cumulative checkpoint, rebuild, assert
bit-identical results for 100 random queries.

**Done when:** the equivalence of the delta and cumulative formulations is asserted on
randomized histories, not just the 5-block example.

---

## Phase 4 — Consumers

Scope: `ConsumerManager`, sessions, cursors, full-backup flags, floor computation.

Acceptance (the brief's scenario, made executable):

```
A commits gen 10; B commits gen 15; C commits gen 18
writes gen 19..20
ranges(A @boundary20).contains(union(11..20))        and is not larger than that union
ranges(B @boundary20) == union(16..20)
ranges(C @boundary20) == union(19..20)
B commits (cursor -> 20); A fails (cursor stays 10)
ranges(A @boundary21) == union(11..21)               // A's history was not destroyed by B
```

| Test | Assertion |
|---|---|
| `NewConsumerRequiresFullBaseline` | `beginBackup` on a consumer with cursor 0 returns `full_backup_required`, `getDirtyRanges` returns *everything* or an explicit "full" marker, never an empty set |
| `MaxConsumersEnforced` | 11th registration with `max_consumers=10` ⇒ `OutOfRange` |
| `CommitIsIdempotent` | same `(consumer, backupId)` twice ⇒ `Ok`, cursor unchanged |
| `StaleBackupIdRejected` | backupId ≤ lastBackupId ⇒ `InvalidState` |
| `EpochMismatchInvalidates` | cursor epoch ≠ device epoch ⇒ `full_backup_required` |
| `AbortDoesNotAdvance` | abort then check cursor and ranges are identical to before |
| `TenConsumersConcurrent` | 10 threads, independent boundaries, each union exact |

**Done when:** the invariant "a consumer's ranges are exactly `union(cursor, boundary]`" is
checked against an oracle after every operation in a randomized state-machine test.

---

## Phase 5 — Persistence and recovery

Scope: superblock A/B, delta/cum serialization, WAL, CRC32C, checkpointing, recovery,
quarantine, `flock`.

Acceptance: **the entire crash matrix in
[metadata-format.md](metadata-format.md) §7 (15 rows)**, plus:

| Test | Assertion |
|---|---|
| `CleanRestartPreservesRanges` | ranges identical before/after restart, bit-for-bit |
| `WalPartialTailTruncated` | only the final partial record is dropped; everything before survives |
| `WalMidStreamCorruptionInvalidates` | a valid record after a corrupt one ⇒ device invalidated, not "resumed with a gap" |
| `MissingDeltaInvalidatesConsumersThatNeedIt` | gap in generation chain ⇒ consumers with cursor below the gap are flagged; others unaffected |
| `CorruptCumIsRebuilt` | cumulative CRC failure with intact deltas ⇒ no consumer affected |
| `DoubleOpenRefused` | second process on same metadata dir exits with a clear error |
| `ForeignMetadataRefused` | copied dir from another host ⇒ `metadata_uuid` mismatch |
| `CrashDuringGcPlan` | flags survive, deletes resume, no consumer sees truncated history |
| `FuzzParsers` | 4 libFuzzer targets, no crash, no OOM, no unchecked size |

**Done when:** every crash row has a deterministic test with an injected crash point, and
the suite runs in CI in under ~10 minutes.

---

## Phase 6 — Simulator, CLI, backup workflow

Scope: file-backed device, strict durability mode, crash injection, CLI, multi-device,
`examples/backup_simulation`, `docs/performance.md` with real numbers.

Acceptance:

- The example reproduces the brief's three-consumer scenario end-to-end from the CLI, with
  a verifiable byte-for-byte restore of the *data* (not just the range list) against a
  reference copy taken at each consumer's boundary.
- `--crash-at=<point>` reproduces Phase 5 outcomes from the CLI surface.
- Benchmark report: throughput with/without tracking, seal latency p50/p99, range-query
  latency for 10 consumers, recovery time vs. WAL size, and measured tracking overhead.
- **Only after this phase** may any performance target be written down.

**Done when:** `examples/backup_simulation` passes in CI and its numbers are in
`docs/performance.md`.

---

## Phase 7 — `dm-era` backend

Precondition: Phase 0 report exists and Q1 is answered.

Acceptance:

- Capability probe (8 steps) implemented and tested on at least three distributions.
- Drain loop persists eras into the history store with no gaps under sustained write load
  for 1 hour.
- Reboot test: after a clean reboot, a consumer resumes incrementally with correct ranges.
- Unclean reboot test: the device is invalidated and every consumer is flagged — the
  *designed* outcome, asserted.
- Snapshot coordination test: `suspend → checkpoint → snapshot → resume`, then verify the
  data read from the snapshot matches the reported ranges (byte-level comparison against
  an offline copy).
- The compatibility matrix is filled only with tested rows.

**Done when:** the "does data match the ranges" test passes on real hardware, since that
is the only test that can catch R1.

---

## Phase 8 — Production hardening

Scope: systemd unit, structured logging, config parsing/validation, safe shutdown,
diagnostics (`status`, `verify`, `fsck`-style read-only checker), metrics, security review,
installation/upgrade, migration tooling.

Acceptance:

- `blockchronicle verify --device <uuid>` performs a full read-only validation of
  superblock + chain + WAL + history and reports the same recovery state the engine would.
- Graceful shutdown sets `clean_shutdown` and is verified by an integration test.
- Upgrade test: v1 metadata opened by v2 code either works or refuses with a migration
  instruction — never half-works.
- Security checklist: permissions on `/var/lib/blockchronicle` (0700 root), no path
  construction from untrusted input, `O_NOFOLLOW`, size validation before allocation,
  refusal to run on non-local filesystems.
- Documented, tested uninstall/`--purge` path that removes metadata only on explicit request.

---

## Cross-phase definition of done

A change is not mergeable unless all of:

1. A test exists that fails without it.
2. The invariant it protects is written down in `docs/` with a link from the code comment.
3. ASan/UBSan clean; TSan clean if it touches shared state.
4. No new warning at the project warning level.
5. No new file format field without a format-version decision.
6. No claim of "production-ready" in any doc without a corresponding test referenced.
