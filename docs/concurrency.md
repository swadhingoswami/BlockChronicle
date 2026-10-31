# Concurrency and Locking

The brief lists eight scenarios that must be correct. This document defines the
mechanism once, then maps each scenario onto it.

---

## 1. Lock inventory and ordering

| # | Lock | Type | Protects | Held during I/O? |
|---|---|---|---|---|
| L1 | `Engine::devicesMutex_` | `std::shared_mutex` | the device registry map | never |
| L2 | `DeviceContext::stateMutex_` | `std::shared_mutex` | active generation, bitmap pointer, floor | seal: yes (bitmap serialization is excluded — see §3) |
| L3 | `DeviceContext::consumerMutex_` | `std::mutex` | consumer records, sessions | no (fsync happens after release, with a re-validation) |
| L4 | `Persistence::walMutex_` | `std::mutex` | WAL append + fsync | yes (short: ≤ 8 KiB record + fdatasync) |
| L5 | `Persistence::superblockMutex_` | `std::mutex` | A/B slot writes | yes (4 KiB + fdatasync) |
| L6 | `RecoveryManager::globalMutex_` | `std::mutex` | startup only | no |

**Ordering (always acquire in increasing number, never skip backwards):**

```
L1 → L2 → L3 → L4 → L5
```

Rules that make the ordering enforceable:

- `L6` is acquired before any device exists; it never nests with L1.
- No lock is held across a call that acquires a *lower*-numbered lock. Verified by
  inspection in review and by a debug-only lock-deputy wrapper (`LockGuard` that records a
  thread-local stack of held order ids and `assert`s monotonicity) compiled in tests only.
- Persistence calls are the only place L4/L5 are taken, and they never call back into the
  engine. This is the "no callbacks under lock" rule.

Why not one global lock: `getDirtyRanges` on ten consumers across three devices while
writes continue is a realistic workload; a global lock serializes bitmap scans with the
mark path and turns a read-only query into a write-path stall.

Why not per-word atomics for everything: marks need it (§2), but everything else needs
group atomicity (seal must see a consistent bitmap). Atomics alone cannot express that.

---

## 2. The mark path

Marks are the hottest path and must not take a mutex.

```cpp
// ActiveBitmap: published under L2
struct ActiveBitmap {
    std::unique_ptr<std::atomic<Word>[]> words;  // C++17: atomic array, fetch_or relaxed
    std::uint32_t regionCount;
    std::uint32_t tailMaskBits;
    Generation    generation;
};

void markRegions(ActiveBitmap& b, std::uint32_t first, std::uint32_t last) {
    // caller holds L2 in shared mode
    for (std::uint32_t w = first / 64; w <= last / 64; ++w) {
        std::uint32_t lo = (w == first / 64) ? first % 64 : 0;
        std::uint32_t hi = (w == last  / 64) ? last  % 64 : 63;
        Word mask = makeMask(lo, hi);
        if (w == last / 64) mask &= tailMaskFor(b);
        b.words[w].fetch_or(mask, std::memory_order_relaxed);
    }
}
```

`fetch_or` with `relaxed` is sufficient because the dirty bit carries no ordering
relationship to any other memory. What *does* need ordering is the seal: it must observe
every `fetch_or` that happened before the writer released its shared L2 lock — and the
`shared_mutex` release/acquire pair provides exactly that (it is a release/acquire
synchronisation, so all prior relaxed atomics are visible). This is the standard
"locks for publication, atomics for the hot path" pattern.

Alternative considered: a seqlock over a plain (non-atomic) bitmap. Faster to read
(no atomic ops), but a seqlock requires the *reader* to retry on conflict, and the seal
reader is the one that must not fail. Rejected: the failure mode would be a seal that
retries indefinitely under sustained write pressure. `fetch_or` has no such problem.

Alternative rejected: per-shard mutexes. Complexity without benefit at these rates;
`fetch_or` on a contended word is ~20 ns.

---

## 3. Rotation vs. mark (the critical protocol)

```mermaid
sequenceDiagram
    participant W1 as Writer 1
    participant W2 as Writer 2
    participant D as DeviceContext (L2 exclusive)
    participant P as Persistence

    W1->>D: shared lock
    W2->>D: shared lock
    W1->>W1: fetch_or bit (gen G)
    D->>D: exclusive lock requested (WAITS)
    W1->>D: unlock
    W2->>W2: fetch_or bit (gen G)
    W2->>D: unlock
    D->>D: exclusive acquired: no marks in flight
    D->>D: copy atomic words -> DirtyBitmap (gen G)
    D->>P: write gen-G delta + SEAL  (durable)
    P-->>D: ok
    D->>D: publish sealed D_G; new ActiveBitmap (gen G+1)
    D->>D: unlock (exclusive)
    W1->>D: shared lock -> fetch_or into gen G+1
```

Guarantees:

- **No mark is split across generations.** A writer holds a shared lock for the duration
  of one mark call; seal needs the exclusive lock, so it waits for all in-flight marks.
- **No mark is lost at rotation.** The copy happens while exclusive.
- **Sealed bitmaps are immutable** and published as `std::shared_ptr<const DirtyBitmap>`.
  Readers (`getDirtyRanges`, comparison, GC planning) hold only a shared lock long enough
  to copy the pointer out, then work without any lock.
- **Seal does not need to hold the exclusive lock during the fsync if latency matters.**
  Two-phase variant for later: (a) swap active→new pointer and capture the frozen copy
  under exclusive, (b) release, (c) fsync, (d) take exclusive again and publish the
  sealed entry. During phase (b)–(c) the generation is "sealed but not visible" and
  sessions must not use it; a session that asks for a boundary in that window waits on a
  condition variable keyed by `last_sealed`. Start with the simple version (fsync under
  the exclusive lock) because seal happens a few times per backup cycle; optimize only
  after measuring.

Cost of the simple version: writers stall for roughly `write + fdatasync` of a 2 MiB
delta (≈ 1–10 ms on SSD). With one backup per day and periodic seals every ~64 generations
this is immaterial. Document the number; do not guess it.

---

## 4. Scenario → mechanism

| # | Scenario from the brief | Mechanism | Test |
|---|---|---|---|
| 1 | many threads, different regions | independent `fetch_or` on different words | TSan + 1 M marks, 8 threads |
| 2 | many threads, same region | idempotent `fetch_or` | count == 1 afterwards |
| 3 | comparison during writes | comparison reads `shared_ptr<const DirtyBitmap>` snapshots only | 4 writers + 4 comparers for 30 s, assert every comparison sees a monotonic prefix |
| 4 | rotation during active writes | §3 exclusive/shared protocol | randomized marks + seals; assert `OR(D1..Dn)` equals the union of all marked regions |
| 5 | history load during GC | GC holds `L2` exclusive only to update the retained-range map; files are unlinked after the map update, and loaders validate `oldest_retained` first and retry once on `ENOENT` | inject unlink between plan and load |
| 6 | many consumers querying | shared L2 only for pointer grabs; no `L3` | 10 consumers × 100 queries |
| 7 | detach during active session | detach takes `L2` exclusive, marks device `Detaching`, fails sessions, then releases; `L1` used only to look up the context | detach mid-query and mid-commit |
| 8 | checkpoint during tracking | checkpoint takes `L2` exclusive for the state copy, writes files outside the lock, and re-validates `last_sealed` before committing the superblock | checkpoint storm + writes |

The rule behind rows 3, 5, 8: **long I/O never happens under a lock that the write path
needs.** Only the state copy is under the lock.

### Consumer commit and the L3/L4/L5 nesting

```
lock L3
  validate session still open, boundary still retained
  build CONSUMER_COMMIT record
  lock L4
    append + fdatasync
  unlock L4
  apply in-memory cursor update
unlock L3
```

If the record is written but the fdatasync fails, do **not** apply the in-memory update
and return an error; the consumer retries and idempotency (`consumer, backupId`) handles
the duplicate. If the write succeeds but the process dies before the in-memory update,
replay fixes it. Both directions are safe because the log is the truth.

---

## 5. Threading model, explicitly

- No detached threads. All threads are owned by a `ThreadPool` with `join()` in the
  destructor, or are the caller's threads (library mode).
- v1 service model: one **ingest** thread per backend that needs polling (dm-era drain),
  one **maintenance** thread (periodic seal + checkpoint + GC), and request handling from
  the IPC/CLI layer. Three threads, not thirty. A `std::condition_variable` driven loop
  with a stop flag, no sleep-polling.
- Long-running scans (`getDirtyRanges` over 1000 deltas) are chunked and check a
  cancellation token so shutdown is bounded; `commitBackup` and `seal` are never chunked
  because they must be atomic.

---

## 6. Sanitizer and stress plan

| Tool | Scope | Gate |
|---|---|---|
| ASan + UBSan | all unit/integration tests, `-DBC_SANITIZE=address,undefined` | every CI run |
| TSan | `tests/concurrency/*`, dedicated CI job | every CI run |
| `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow` | everything | warnings are errors |
| clang-tidy | `bugprone-*`, `cppcoreguidelines-*` (subset), `performance-*` | advisory first, then blocking |
| fuzz (libFuzzer) | header parsers: delta, WAL record, superblock, consumer record | nightly, 10 min per target |

Fuzz targets are mandatory for Phase 5 acceptance. Every parser is a fuzz target, because
every parser reads data that can be corrupt by definition.

Stress shapes (Phase 6):

- 64 threads × 10 M random 4 KiB writes over a 1 TiB virtual device, seals every 100 k
  writes: assert final union contains every marked region (compare against a
  single-threaded oracle computed from the write log).
- 10 consumers with staggered cursors, random commits/aborts, random seals, random GC: the
  invariant checked after every step is
  `ranges(consumer) ⊇ oracle_union(cursor, boundary)`.
- Concurrent `compareGenerations` while GC deletes: no use-after-free (ASan), no
  `ENOENT`-induced wrong answer (must retry or invalidate, never return empty).

---

## 7. Ownership summary

| Object | Owner | Lifetime |
|---|---|---|
| `DeviceContext` | `Engine` (unique_ptr in map) | attach..remove |
| `ActiveBitmap` | `DeviceContext`, swapped on seal | one generation |
| sealed `DirtyBitmap` | `shared_ptr<const>` in `HistoryStore` | until GC |
| `BackupSession` | `ConsumerManager` | begin..commit/abort |
| `SnapshotHandle` | `SnapshotCoordinator` | create..release |
| WAL fd / segment | `Persistence` | process lifetime |
| `TrackingBackend` | `DeviceContext` | attach..detach |
