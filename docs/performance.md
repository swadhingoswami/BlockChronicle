# Performance — Baseline Measurements

**No targets are stated here.** These are measurements from one machine, taken to
establish a starting point before any optimization or claim. Re-run `bc_benchmark` on
the target hardware before quoting any number.

---

## Method

`tools/benchmark.cpp`, built as `bc_benchmark`. It creates a 256 MiB file-backed device
(4096 regions at 64 KiB), exercises each phase, and prints a table.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bc_benchmark
```

Each row is a single run's mean. Three consecutive runs are reported where variance
mattered.

## Environment

| Item | Value |
|---|---|
| Host | Apple Silicon, macOS, APFS |
| Build | `RelWithDebInfo`, AppleClang |
| Device | file-backed simulator, 256 MiB |
| Consumers | 10 |

## Results

| Measurement | Run 1 | Run 2 | Run 3 |
|---|---:|---:|---:|
| Marked 64 KiB writes (journaled, fsync each) | 40 523 ops/s | 31 603 ops/s | 29 203 ops/s |
| per write | 0.025 ms | 0.032 ms | 0.034 ms |
| Seal, 4096-region bitmap | 0.200 ms | 0.249 ms | 0.198 ms |
| Seal, 1 dirty region | 0.175 ms | 0.173 ms | 0.169 ms |
| Begin backup (seal + 256 MiB snapshot copy) | 100.6 ms | 115.0 ms | 117.0 ms |
| **Enumerate dirty ranges** (188 ranges, 10 consumers) | **0.0035 ms** | **0.0038 ms** | **0.0038 ms** |
| Checkpoint (50 generations of history) | 3.0 ms | 4.1 ms | 3.6 ms |
| Cold open (load 50 deltas + replay) | 18.5 ms | 16.5 ms | 17.4 ms |
| Snapshot copy of the 256 MiB image | 150.9 ms | 153.5 ms | 177.5 ms |

## Interpretation

**Range enumeration is effectively free.** 3.5 µs for 188 coalesced ranges across ten
consumers. This is the payoff of the Phase 3 correction: the answer is a property of the
consumer's pending bitmap, so a query is a read plus a scan, not a walk over history.
The cost is paid at seal time instead, where it is bounded and predictable.

**Sealing costs about 0.2 ms** for a 4096-region bitmap, independent of how many regions
are dirty, because it serializes the whole bitmap. At 1 TiB (16.7 M regions, 2 MiB
bitmap) expect roughly 30–50× that, in the low milliseconds. Seals happen a few times per
backup cycle, so this is not on the write path.

**Beginning a backup is dominated by the snapshot**, not by the engine. 100–120 ms for a
256 MiB copy at ~1.5 GiB/s. This is a property of the simulator's deliberately simple
snapshot, and it is exactly why a real backend must use `dm-snapshot` or `dm-thin`
(instant, copy-on-write) rather than a copy. The number is useful as an upper bound on
what "the engine added" — it added almost nothing.

**Cold open scales with history, not with device size**, in this test: 17 ms to load 50
deltas and replay the journal. At 1 TiB each delta is 2 MiB, so 1000 deltas would mean
~2 GiB of reads; the design's bytes-based retention ceiling exists for this reason.

**Checkpoint cost is fsync-dominated and has a long tail.** Most runs are 3–4 ms; one
earlier run measured 109 ms. Single-sample fsync numbers are not trustworthy; treat the
distribution, not the mean, as the measurement.

## The caveat that matters most

**These write numbers do not represent durable writes on Linux.**

`fsync` on macOS does not flush the drive's write cache (that requires
`F_FULLFSYNC`). The 29k–40k writes/second with one journal record per write therefore
measures the page cache and the filesystem log, not the device. On Linux with a real
storage stack, one `fdatasync` per write costs on the order of 0.1–10 ms depending on
media, so the same workload should be expected to be **one to three orders of magnitude
slower**.

Consequences, stated plainly:

1. The simulator's per-write journaling (`journalMarks = true`) is a correctness device
   for a test backend, not a performance baseline.
2. No conclusion about production write throughput can be drawn from this table.
3. The rows that *are* meaningful across platforms are the CPU-bound ones: range
   enumeration, bitmap operations, and seal serialization.

## What to measure next

1. The same benchmark on Linux with `fdatasync`, to get the real journaling cost.
2. A `dm-era` drain loop at 1 TiB, comparing era-read latency against the 3.5 µs query
   here.
3. Seal latency at 1 TiB and 10 TB bitmap sizes.
4. Recovery time with 1000 deltas and a 1 GiB journal.
5. Sustained 10-consumer query load while writes continue (concurrency, not single-thread
   throughput).

Each of these belongs in this document with its own dated run, not in a design meeting.
