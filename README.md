# BlockChronicle

**A persistent Linux block-level change tracking engine.**

BlockChronicle records which regions of a block device changed, preserves that history
durably across restarts, and answers per-consumer queries of the form:

> "Give me every region that changed on this device since *my* last successful backup."

One tracked device serves many independent backup consumers, each with its own cursor.
No consumer's success may destroy another consumer's history.

**Status: Phase 0 — architecture review. No production code yet.**

---

## Documents

| Document | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Product spec, component model, module responsibilities, responsibility split, API sketch, repository layout |
| [docs/consistency-model.md](docs/consistency-model.md) | Generations, boundaries, snapshot coordination, write-tracking ordering, durability protocol |
| [docs/bitmap-history.md](docs/bitmap-history.md) | Delta vs. cumulative bitmaps, comparison semantics, sparse encoding, retention and garbage collection |
| [docs/metadata-format.md](docs/metadata-format.md) | On-disk layout, superblock, WAL, checkpointing, crash matrix, recovery algorithm |
| [docs/concurrency.md](docs/concurrency.md) | Lock inventory, lock ordering, rotation-vs-mark protocol, snapshot publication |
| [docs/linux-backend.md](docs/linux-backend.md) | `dm-era` feasibility assessment, scaling math, fallback options, custom target sketch |
| [docs/risks.md](docs/risks.md) | Risk register, unresolved questions, assumptions that must be tested |
| [docs/roadmap.md](docs/roadmap.md) | Phase 0–8 plan with concrete, falsifiable acceptance criteria |

## Non-goals for v1

- No macOS/Windows support.
- No network management server, cloud integration, or GUI.
- No application-consistent backup guarantees (crash-consistent only, and only with a snapshot).
- No external database engine.
- No "smart" repair of corrupt metadata. Corruption degrades to *full backup required*.

## The ten invariants

Any implementation that violates one of these is wrong, regardless of test results.

1. Never lose an acknowledged write from tracking history.
2. Never advance a consumer cursor before its backup succeeds.
3. Never delete history still required by any consumer.
4. Never return incomplete dirty ranges as a valid incremental result.
5. Never conflate backup IDs with tracking generation IDs.
6. Never treat bitmap comparison as recovery of old block contents.
7. Never treat a dirty bitmap as a consistent snapshot.
8. Never silently switch to an incomplete backend.
9. Never assume a distribution provides `dm-era` without runtime validation.
10. If tracking correctness cannot be proven, require a full backup.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).

Rationale: permissive, includes an explicit patent grant, and keeps the option of
releasing a Device Mapper target under GPL-2.0 in `kernel/` later, which is the usual
arrangement for storage projects that ship both userspace tools and a kernel component.

## Build

Phase 1 builds on any POSIX host, including macOS, because `DirtyBitmap` and `Geometry`
are platform-neutral C++17. Linux-only code appears at Phase 7 behind the
`TrackingBackend` interface.

Requires CMake ≥ 3.20 and a C++17 compiler (GCC 11+, Clang 14+). GoogleTest is fetched
automatically if not installed.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Sanitizer builds:

```bash
cmake -S . -B build-asan -DBC_SANITIZE=address,undefined && cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DBC_SANITIZE=thread && cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```
