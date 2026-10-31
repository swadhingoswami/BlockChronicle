# Implementation Status

Tracks what is actually implemented and verified, as opposed to designed.
Updated when a phase's acceptance criteria are met.

**Rule:** a phase is listed as done only with the command that proves it.

---

## Phase 0 — Architecture review: **done**

Deliverable: `docs/*.md`, committed in `9a06708`.

Not done (belongs to Phase 0 by the roadmap but requires Linux hardware):
the nine `dm-era` measurements in `linux-backend.md` §2 and the answers to
experiment table in `roadmap.md`. Those remain open and block Phase 7, not Phase 1–6.

---

## Phase 1 — DirtyBitmap: **done**

Deliverable: `include/blockchronicle/{types,bitmap}.h`, `src/core/bitmap.cpp`,
`tests/unit/test_bitmap.cpp` — commit `3a3f83a`.

Verified:

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
# 29/29 pass; also 29/29 under address,undefined and under thread
```

Deliberate deviation: the two concurrent-marking tests were moved to Phase 2, where
`ActiveBitmap` exists. `DirtyBitmap` is intentionally not thread-safe
(`concurrency.md` §2).

---

## Phase 2 — Generation lifecycle: **done (in-memory)**

Deliverable: `include/blockchronicle/generation.h`, `src/core/generation.cpp`,
`tests/concurrency/test_generation.cpp`, `src/core/word_math.h`.

Verified:

```bash
ctest --test-dir build        # 41/41
ctest --test-dir build-asan   # 41/41, ASan+UBSan clean
ctest --test-dir build-tsan   # 41/41, no TSan reports
```

Coverage of the roadmap's Phase 2 list:

| Criterion | Test |
|---|---|
| Seal publishes exactly the closing generation's bits | `SealPublishesTheFrozenDelta` |
| Writes after a seal go to the next generation | `WritesAfterASealGoToTheNextGeneration` |
| Failed seal publishes nothing and preserves bits | `FailedSealLosesNothingAndRetrySucceeds` |
| Retry after failure loses nothing (brief steps 1–9) | `FailedSealLosesNothingAndRetrySucceeds` |
| No misattribution after a failed seal | `MarkAfterFailedSealIsNotMisattributed` |
| Seal waits for in-flight marks | `MarkInFlightDuringSealIsNotLost` |
| No mark lost or duplicated across seals | `NoMarkLostAcrossConcurrentSeals` |
| Sealed deltas are immutable | `SealedDeltaIsImmutableByType` (compile-time) |
| Periodic seal bound | `SealIfDirtyAtLeastRespectsTheThreshold` |
| Duplicate marks on the same region | `ConcurrentMarksOnTheSameRegionAreIdempotent` |

### Mutation check (evidence the tests have teeth)

Removing `std::shared_lock` from `GenerationManager::markRegion` makes
`MarkInFlightDuringSealIsNotLost` fail, with the marked region present in no generation
at all. The test detects a lost write, not merely a different ordering. Any future change
to the mark/seal protocol should be re-validated this way.

### Deferred, with reasons

| Item | Reason |
|---|---|
| `SealIsIdempotent` for the same generation number | In-memory `seal()` always advances; idempotency is a property of WAL replay keyed by generation, which is Phase 5. |
| Multiple frozen boundaries | Explicitly out of scope per the brief until the single-boundary design is proven on hardware. |
| Persistence of deltas | `SealSink` is the seam. `RecordingSink` in the tests is a stand-in for the Phase 5 WAL/delta writer. |
| Crash between durable commit and publication | Requires the WAL; Phase 5 crash matrix rows 4 and 7. |

---

## Phase 3 — History semantics: **done**

Deliverable: `include/blockchronicle/history.h`, `src/core/history.cpp`,
`include/blockchronicle/seal.h`, `tests/unit/test_history.cpp`.

Sealed history moved out of `GenerationManager` into `HistoryStore`, which owns the
deltas, the checkpoint grid and retention. `GenerationManager` publishes into it, so there
is exactly one owner of history and one owner of the active generation.

Verified:

```bash
ctest --test-dir build   # 60/60
```

Coverage:

| Criterion | Test |
|---|---|
| `(from, to]` exclusive/inclusive semantics | `UnionIsExclusiveFromInclusiveTo` |
| Empty interval is Ok and empty | `EmptyIntervalIsOkAndEmpty` |
| Unsealed boundary refused, never partial | `UnsealedBoundaryIsNotFoundNotPartial` |
| Reclaimed history refused | `ReclaimedHistoryIsNotFound` |
| Non-contiguous commit rejected | `NonContiguousCommitIsRejected` |
| Durable-commit failure is not published | `FailedDurableCommitIsNotPublished` |
| Checkpoint grid at interval K | `CheckpointsAppearEveryInterval` |
| Checkpoints rebuildable, results unchanged | `RebuildCheckpointsProducesIdenticalResults` |
| Union is composable across split points | `UnionIsComposableAcrossSplitPoints` |
| Union matches the naive oracle on random history | `UnionMatchesNaiveOracleOnRandomHistory` |
| Queries stay correct after reclaiming history | `QueriesStayCorrectAfterReclaimingTheOldestGeneration` |
| **Cumulative difference is not the incremental set** | `CumulativeDifferenceIsNotTheIncrementalUnion` |

### Architecture finding: cumulative bitmaps are analysis-only

The Phase 3 implementation initially used `Cum_b AND NOT Cum_a` to answer range queries,
as the design documents specified. The property test found 54 missing regions in range
`(99, 120]`: the formula computes "first changed in the interval", which drops every
region changed on both sides of the cursor — a false negative, i.e. silent data loss.

The implementation now computes the definition directly. Cumulatives remain available for
analysis ("never changed since baseline") but never feed a range query. `docs/risks.md`
§4 F1 records the finding; `architecture.md`, `consistency-model.md`, `bitmap-history.md`
and `roadmap.md` were corrected.

Consequence: the acceleration for Phase 4 is a **per-consumer pending bitmap**, not
checkpoint replay. Cost is `O(consumers × bitmap)` per seal and `O(1)` per query.

---

## Phase 4 — Consumers: **done (in-memory)**

Deliverable: `include/blockchronicle/consumer.h`, `src/core/consumer.cpp`,
`tests/unit/test_consumer.cpp`, `tests/concurrency/test_backup_concurrency.cpp`.

Verified:

```bash
ctest --test-dir build        # 76/76
ctest --test-dir build-asan   # 76/76
ctest --test-dir build-tsan   # 76/76, no reports
```

Design as corrected in Phase 3: each consumer owns a **pending bitmap**, the OR of every
delta sealed since its cursor, updated at seal time and rebuilt at commit. Queries are
O(1) reads of an immutable snapshot; the device-wide cumulative checkpoints play no part.

### The subtlety that drove the implementation

Pending accumulates *past* an in-flight session's boundary, and OR cannot be subtracted.
So committing at boundary `B` when seals have continued to `B+n` cannot simply clear
pending (that loses `B+1..B+n`) nor keep it (that re-sends everything up to `B`). Each
consumer therefore retains shared references to the unacknowledged deltas and rebuilds
pending from the survivors on commit.

Mutation-verified in both directions: `LateCommitRebuildsPendingWithoutDuplication` fails
if commit clears pending, and fails again if commit never drops acknowledged deltas.
Either mistake is a production data bug and neither is visible without the late-commit
scenario.

Coverage:

| Criterion | Test |
|---|---|
| Duplicate / malformed / excess registration | `RegistersAndRejectsDuplicates`, `EnforcesMaxConsumers`, `RejectsMalformedIdentifiers` |
| New consumer must baseline first | `NewConsumerRequiresAFullBaseline` |
| Full session has no range list | `NewConsumerRequiresAFullBaseline` |
| Ranges = union since own cursor | `IncrementalRangesCoverExactlyTheGenerationsSinceTheCursor` |
| **Late commit loses nothing, re-sends nothing** | `LateCommitRebuildsPendingWithoutDuplication` |
| Abort never advances | `AbortNeverAdvancesTheCursor` |
| Stale backup ids and session reuse rejected | `CommitRejectsStaleBackupIdsAndReuse` |
| The brief's A/B/C scenario, including A failing | `IndependentHistoriesSurviveAnotherConsumersSuccess` |
| Ten independent cursors | `TenConsumersKeepIndependentCursors` |
| Invalidation forces full backups | `InvalidationForcesFullBackupsAndKillsOpenSessions` |
| Concurrent sessions under concurrent seals | `SessionsSeeExactlyTheirOwnBoundaryUnderConcurrentSeals` |

### Deliberate deviations

| Decision | Reason |
|---|---|
| `beginBackup` lives on `GenerationManager`, not a separate `Engine` | It is the only operation that must hold the device lock across boundary capture; introducing an `Engine` now would be a pass-through with a lock bug waiting to happen. Phase 6 adds `Engine` on top. |
| Commit is not idempotent at this layer; the session is invalidated | Crash-safe idempotency belongs to WAL replay keyed by `(consumer, backupId)` (Phase 5). Two layers both trying to be idempotent is worse than one. |
| Session `pending` is a shared_ptr snapshot, not a copy | 2 MiB memcpy per session avoided; the bitmap is immutable so sharing is safe. |

### Known gap introduced here (Phase 5 must close it)

Consumer state is in memory only. `ConsumerManager` holds shared references to deltas but
has no `HistoryStore` reference, so after a restart it cannot rebuild `pending`. Phase 5
must either persist consumer records or give the manager a rebuild path from
`(cursor, history)`. Until then, a restart is equivalent to invalidation, which is safe
but expensive.

---

## Not yet started

Phase 5 (persistence/recovery), Phase 6 (simulator, CLI), Phase 7 (dm-era),
Phase 8 (hardening).

## Known gaps in the current code

- `ActiveBitmap::countDirty()` is a snapshot under concurrent marking. It is used only
  for the periodic-seal heuristic, never as a durability signal. `seal()` re-reads every
  word under the exclusive lock, so published deltas are exact.
- `GenerationManager` has no persistence, no consumers and no retention policy. It
  deliberately knows nothing about either.
- No CI configuration yet. Three local configurations are verified by hand; automating
  them is a Phase 5 prerequisite, not a nicety.
