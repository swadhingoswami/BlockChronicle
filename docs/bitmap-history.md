# Historical Bitmap Model

---

## 1. Data structures

```cpp
// include/blockchronicle/bitmap.h
namespace bc {

using Word = std::uint64_t;

constexpr std::uint32_t kDefaultGranularity = 64u * 1024u;

class DirtyBitmap {
public:
    DirtyBitmap(std::uint32_t regionCount, std::uint32_t granularity);

    std::uint32_t regionCount() const;
    std::uint32_t granularity() const;
    std::uint64_t capacityBytes() const;

    // Marking. Returns false if the range is invalid (overflow, beyond capacity).
    // Thread-safe: shared-lock semantics are the caller's responsibility (see concurrency.md).
    void markRegion(std::uint32_t index);
    void markRegionRange(std::uint32_t first, std::uint32_t lastInclusive);

    bool test(std::uint32_t index) const;

    std::uint64_t countDirty() const;               // popcount over words, tail-masked
    bool any() const;
    bool none() const;

    // Bitwise ops. All require identical regionCount + granularity.
    static DirtyBitmap bitwiseAnd(const DirtyBitmap&, const DirtyBitmap&);
    static DirtyBitmap bitwiseOr(const DirtyBitmap&, const DirtyBitmap&);
    static DirtyBitmap bitwiseAndNot(const DirtyBitmap& a, const DirtyBitmap& b); // a & ~b
    static DirtyBitmap bitwiseXor(const DirtyBitmap&, const DirtyBitmap&);

    // Enumeration without materializing per-block lists.
    void forEachRange(RangeVisitor& visitor) const;  // coalesced [firstRegion..lastRegion]
    std::vector<DirtyRange> toRanges(std::uint64_t maxRanges) const;

    // Storage-agnostic access for persistence.
    const std::vector<Word>& words() const;
    void setWords(std::vector<Word>&&);

private:
    std::uint32_t      regionCount_;
    std::uint32_t      granularity_;
    std::uint32_t      tailMaskBits_;   // valid bits in the last word
    std::vector<Word>  words_;
};

} // namespace bc
```

Design decisions and their justification:

- `std::vector<uint64_t>`, not `std::vector<bool>`. Non-negotiable: `vector<bool>` has no
  portable word access, no `data()`, and serializes badly.
- `tailMaskBits_` is stored explicitly so the final partial region can never be marked
  accidentally, and so `countDirty` is exact. All bit operations mask the tail after
  every `AND`/`OR`/`NOT`, otherwise `A AND NOT B` can set bits above the tail and inflate
  counts or produce out-of-range ranges.
- No thread safety inside the class. Synchronisation lives one level up, where it can be
  shared across a group of bitmaps. A per-bitmap mutex would make bitmap *comparison*
  impossible to do atomically.
- `forEachRange(RangeVisitor&)` uses an interface, not `std::function`, to keep the hot
  path allocation-free and to avoid the lambda style the project avoids.

`RangeVisitor` is small and explicit:

```cpp
class RangeVisitor {
public:
    virtual ~RangeVisitor() = default;
    // Region indices inclusive. Implementations coalesce or filter as needed.
    virtual void onRegionRange(std::uint32_t first, std::uint32_t last) = 0;
};
```

### Region math (overflow-safe)

```cpp
// region count for a device
regionCount = capacity / G + (capacity % G != 0 ? 1 : 0);   // capacity > 0

// write [offset, offset+len) touches regions:
firstRegion = offset / G;
lastByteExclusive = offset + len;                            // must not overflow: check len <= capacity - offset
lastRegion  = (lastByteExclusive - 1) / G;                   // len > 0 only
```

Validation rules, each with a dedicated test:

| Input | Result |
|---|---|
| `len == 0` | no-op, `Ok` (not an error — POSIX zero-length writes are legal) |
| `offset >= capacity` with `len > 0` | `OutOfRange` |
| `len > capacity - offset` | `OutOfRange` (covers overflow, since capacity ≤ 2^64-1) |
| `offset == capacity`, `len == 0` | `Ok`, no-op |
| `capacity == 0` | device rejected at registration |

Note: `offset + len` overflow is avoided by comparing against `capacity - offset` rather
than adding. A test must cover `offset = 2^64-8, len = 16`.

---

## 2. Two bitmap kinds, never interchangeable

```cpp
enum class BitmapKind : std::uint8_t {
    Delta      = 1,   // written during generation G only
    Cumulative = 2,   // written at any generation in [baselineGen, G]
};
```

`Cumulative` **must** carry `baselineGen`. `Delta` must not (it is implicitly
`[G, G]`). The persistence layer refuses to load a cumulative bitmap whose baseline is
missing or greater than its generation, and refuses to diff two cumulative bitmaps with
different baselines or epochs.

| Need | Use |
|---|---|
| Union since consumer cursor | `OR` of deltas, or `Cum_B AND NOT Cum_C` with a shared baseline |
| "What changed *in* generation G" | `D_G` |
| GC decision | deltas only (they are the unit of retention) |
| Rebuild after corruption | deltas only |
| Fast path for long spans | cumulative checkpoints every `K` seals |
| Audit / user-saved snapshot | explicit `saveSnapshot` copy, labelled, never used by the engine's logic |

---

## 3. Why both, and what each one can and cannot do

Deltas alone answer every incremental question, at a cost proportional to the number of
generations in the interval: "since generation 10" at generation 1000 costs 990 `OR`s of up
to 2 MiB each.

The obvious acceleration is wrong, and it is worth stating precisely because it is the most
dangerous simplification in this domain.

### 3.1 `Cum_b AND NOT Cum_a` is not "changed since a"

Let `Cum_k = OR(D_1..D_k)`. Then:

```
Cum_b AND NOT Cum_a = { r : r changed somewhere in (a, b] AND r never changed in [1, a] }
```

That is "**first** changed in `(a, b]`", not "changed in `(a, b]`". A region that changed
both before and after the consumer's cursor is dirty in both cumulatives and cancels out.

A consumer at cursor `a` already captured the data as of `a`. It must receive every region
that changed in `(a, b]`, **including** regions that also changed earlier. Dropping them is
a false negative — silent data loss: the backup completes, reports success, and is missing
blocks.

This was found during Phase 3, not in review. The property test
`HistoryStore.CumulativeDifferenceIsNotTheIncrementalUnion` shows a region changing in both
periods and disappearing from the difference. No choice of checkpoint density or baseline
fixes it, because the information ("did this region change in this interval?") has already
been collapsed away.

**Rule: a device-wide cumulative bitmap must never be used to compute an incremental
range.** Any code that feeds `bitwiseAndNot(cumulativeTo, cumulativeFrom)` into a range
query is a data-loss bug. `HistoryStore::unionRange` therefore computes the definition
directly and carries a comment pointing at this section.

### 3.2 The correct acceleration is per consumer

"Changed since C" depends on C, so any exact acceleration must be parameterised by C. The
only structure that works is a **pending bitmap per consumer**:

```
Pending_c  = OR of every delta sealed since consumer c's cursor
query(c)   = Pending_c                              -- O(1), no computation
seal(D_g)  = for each consumer c: Pending_c |= D_g   -- O(consumers) ORs per seal
commit(c)  = Pending_c := empty
```

- Cost per seal: `consumers × bitmap`. At 10 consumers and 2 MiB, 20 MiB of OR per seal, a
  few times per backup cycle. Bounded, and independent of history depth.
- Cost per query: zero. Strictly better than checkpoint replay.
- Memory: `consumers × bitmap`. 10 × 18 MiB at 10 TB = 180 MiB, configurable.
- Rebuild: OR the deltas from the consumer's cursor. Deltas remain the source of truth, so
  the pending bitmap is a cache and can be discarded at any time.

This replaces the checkpoint-replay scheme that the earlier draft of this document
proposed. It is implemented in Phase 4 (`ConsumerManager`); Phase 3 shipped the definition
and the deltas it depends on. See `docs/implementation-status.md`.

One consequence worth stating here because it is easy to get wrong in review: `pending`
accumulates past an in-flight session's boundary, and OR cannot be subtracted. On commit
at boundary `B`, the consumer rebuilds `pending` from the retained unacknowledged deltas
rather than clearing or keeping it. Both naive alternatives are data bugs and both are
covered by mutation-tested regression tests.

### 3.3 Then what are cumulative checkpoints for?

`Cum_k` keeps legitimate **analysis** uses, none of which feed an incremental query:

- "Which regions have **never** changed since the baseline?" (`NOT Cum_k`) — describes a
  device's churn footprint and sanity-checks a full baseline.
- Cross-checking an incremental result: every block in `union(a,b)` must satisfy
  `Cum_b` (a free invariant, not a query implementation).
- Diagnostic comparison of two historical states (`saveSnapshot`), where "first changed in
  the interval" is the intended meaning.

They are derivable, deletable, and must never be the only record of anything.

### 3.4 Consequence for retention

Because the pending bitmap is per consumer and rebuildable, GC never needs cumulative
checkpoints for correctness. Deleting a checkpoint costs analysis capability only.

## 4. Comparison operations

The brief asks for `A AND B`, `A AND NOT B`, `B AND NOT A`, `A OR B`. All four are
trivial over words. What matters is the **semantics table**, because "compare" is
ambiguous and the wrong interpretation silently produces wrong backups.

| Operation | Meaning | Valid for | Consumer-facing use |
|---|---|---|---|
| `and` | regions dirty in both | deltas only (same generation) or cums (same baseline) | analysis |
| `andNot(a,b)` | dirty in `a`, not in `b` | cums with same baseline; deltas of different gens | analysis only — **never the required set** (§3.1) |
| `or` | union | deltas of any distinct gens; cums same baseline | **union since cursor (definition)** |
| `xor` | changed in exactly one | same as above | analysis |

Deliberately rejected interpretations:

- `A AND NOT B` on two *deltas* (say `D15 AND NOT D10`) is almost never what a backup
  wants: it means "changed in 15 but not in 10", which is not "changed since 10". The API
  must therefore not expose a generic `difference(fromGen, toGen)` that hides which
  bitmap kind it applied. `compareGenerations(from, to)` is defined as `Cum_to AND NOT
  Cum_from` and documented as such; `unionRange(from, to)` is `OR(D_from+1..D_to)`.
  Tests assert these produce identical results when the checkpoints are consistent — that
  identity is a strong self-check and should be a property test.

### Property tests (fixed seeds, run in CI)

- `unionRange(a,b) == XOR-free decomposition`: `unionRange(a,c) == unionRange(a,b) OR unionRange(b,c)` for `a<b<c`.
- `unionRange(a,b) == cum(b) AND NOT cum(a)` after rebuilding cums from deltas.
- `andNot` tail masking: bitmaps whose `regionCount % 64 != 0` never produce bits above
  the tail.
- Idempotence: marking the same region twice yields the same count.
- Commutativity/associativity of `or` over random 1000-bitmap sequences.

---

## 5. Retention and garbage collection

### 5.1 What pins history

A generation `g` must be retained if any of:

1. `g > min(cursor)` over consumers with `fullBackupRequired == false`;
2. `g > session.cursorAtStart` for an open session;
3. `g` is required to rebuild a cumulative checkpoint that a valid consumer will use —
   formally: `g ≥ ckptFloor(min(cursor))`;
4. `g` is part of an operator-pinned analysis range (explicit `saveSnapshot`, which is a
   *copy*, so it does not pin originals — see §5.4).

### 5.2 GC algorithm

```
compute floor from (1)(2)
if floor <= oldestRetained: nothing to do

target = floor
affected = consumers whose cursor < target  OR whose epoch mismatches
                                        OR whose ckpt-grid dependency is broken

phase 1 (durable):  append GC_PLAN{targetEpoch, affected[], targetGen}
                    set full_backup_required=true for each affected
phase 2 (durable):  append GC_DONE{...}
phase 3 (bytes):    unlink deltas with gen <= target, recompute cumulative grid
                    update superblock oldestRetained
```

Crash between phase 1 and 2: on replay, the plan is honoured — the flags are already set,
and the deletion is idempotent. Crash before phase 1's record is durable: nothing was
deleted.

This "intent log before destructive action" pattern is the only acceptable way to delete
history, because it converts a two-phase distributed problem (flag + delete) into a
single durable decision.

### 5.3 Configuration, honestly

| Knob | Effect | Danger |
|---|---|---|
| `max_history_generations` | caps count | may invalidate a lagging consumer |
| `max_history_bytes` | caps size (recommended primary) | same |
| `max_history_age_days` | time-based | **worst knob**; a consumer that is offline for a holiday can silently be pushed to full backup. Keep it, but default it to disabled and log loudly when it fires. |
| `consumer.retentionExempt` | never GC for this consumer | can pin history forever; cap total bytes as a hard stop |

No policy may delete required history without the persisted flag. There is no "force"
flag in v1. If space runs out, the correct behaviour is to fail writes to the *metadata
store* loudly, not to corrupt the incremental chain.

### 5.4 Operational vs. analysis snapshots

Two physically separate namespaces:

- `history/gen-<g>.delta` — operational, GC-controlled, never deleted except by §5.2.
- `snapshots/<uuid>/` — explicit copies created by `saveSnapshot`, with their own
  metadata, their own retention, and no effect on floors.

`deleteSnapshot` touches only the second. A test asserts that deleting every analysis
snapshot leaves every consumer's `getDirtyRanges` output bit-identical.

---

## 6. Sparse encoding

Dense layout (`format flag 0`): `words_` verbatim, `regionCount/64 * 8` bytes.
Simple, fast, 2 MiB at 1 TiB. Correct default for Phase 1.

Sparse layout (`format flag 1`), design:

```
header: regionCount, granularity, dirtyWordCount, nonzeroWordCount
nonzeroIndex:   bit per word, ceil(wordCount/8) bytes
values:         nonzeroWordCount * 8 bytes, in ascending word order
```

- Extracting word `i` requires `rank(nonzeroIndex, i)` → `O(wordCount/64)` if recomputed
  naively, or store a per-512-word prefix-sum array (`SparseRank`) for `O(1)`.
- Cost model at 1 TiB (`wordCount = 262 144`): index = 32 KiB; values = 8 B × nonzero.
  Deltas at 1 % density ≈ 32 KiB + 160 KiB ≈ 192 KiB vs 2 MiB dense (10×).
- Rejected alternatives: RLE over ranges (bad for scattered writes, slower random access),
  general compression (zstd: opaque, defeats random access, adds a dependency),
  Roaring bitmaps (a dependency for a format we control; the two-level scheme is 30 lines
  of code and has the same effect at this scale).

Do not implement sparse encoding until a benchmark shows dense is a problem. The format
flag exists so it can be added without a format break.

---

## 7. Corruption policy per object

| Object | Failure | Consequence |
|---|---|---|
| `D_g`, `g` outside any consumer's `(cursor, boundary]` | CRC mismatch | quarantine file, log, continue. No consumer affected. |
| `D_g`, `g` inside a required interval | CRC mismatch or chain break | those consumers → `full_backup_required`, persisted, then delete the unusable suffix |
| `Cum_k` | CRC / baseline mismatch | ignore it, rebuild from deltas if the deltas exist; no consumer affected |
| `Cum_k` rebuild impossible (deltas gone) | — | consumers whose fast path needed it fall back to the nearest surviving cumulative, or invalidate if none ≤ cursor |
| `D_g` missing entirely, chain gap | — | every consumer with cursor `< g` invalidated. A gap is not a "hole to skip". |
| Analysis snapshot | any failure | delete it, warn |

The last row of the operational table is the one people get wrong: a *missing* file is
worse than a corrupt one, because it is easy to interpret as "nothing changed".
Generation numbers must be contiguous per epoch, and the hash chain must encode the
previous generation's hash, so a gap or a substitution both fail verification
deterministically.

Hash chain:

```
link_g = SHA-256( link_{g-1} || deviceUuid || epoch || g || crc32c(deltaBytes) || regionCount )
```

Stored in both the delta header and the `SEAL` WAL record; verification on load and during
replay. SHA-256 via a small vendored implementation keeps the dependency footprint at
zero (CRC32C, not SHA, is used for per-record integrity; SHA-256 is used only for
tamper/gap detection). If a dependency-free hash is preferred, BLAKE2s is a reasonable
alternative — decide once, in Phase 5, and document the choice in the format spec.

---

## 8. Scale table (measured targets for Phase 6, not invented)

| Capacity | Regions @64 KiB | Dense bitmap | 1000 dense deltas | Sparse @1 % | Sparse @10 % |
|---|---:|---:|---:|---:|---:|
| 500 GB | 7.63 M | 0.95 MiB | 0.95 GiB | ~95 MiB | ~950 MiB |
| 1 TiB | 16.78 M | 2.00 MiB | 2.00 GiB | ~200 MiB | ~2.0 GiB |
| 2 TB | 30.52 M | 3.64 MiB | 3.64 GiB | ~364 MiB | ~3.6 GiB |
| 10 TB | 152.59 M | 18.19 MiB | 18.19 GiB | ~1.8 GiB | ~18.2 GiB |

Interpretation: 1000 *deltas* is only sustainable if most deltas are sparse. The
defaults must therefore not be "1000 generations"; they should be
`max_history_bytes` with a sane default (e.g. 2 GiB per device) plus
`max_history_generations` as an additional bound. A busy 10 TB array will hit the byte
limit long before 1000 generations, and that is the correct behaviour — it triggers
invalidation of lagging consumers *explicitly* rather than filling the filesystem.
