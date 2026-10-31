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

## Not yet started

Phase 3 (history semantics), Phase 4 (consumers), Phase 5 (persistence/recovery),
Phase 6 (simulator, CLI), Phase 7 (dm-era), Phase 8 (hardening).

## Known gaps in the current code

- `ActiveBitmap::countDirty()` is a snapshot under concurrent marking. It is used only
  for the periodic-seal heuristic, never as a durability signal. `seal()` re-reads every
  word under the exclusive lock, so published deltas are exact.
- `GenerationManager` has no persistence, no consumers and no retention policy. It
  deliberately knows nothing about either.
- No CI configuration yet. Three local configurations are verified by hand; automating
  them is a Phase 5 prerequisite, not a nicety.
