# Risk Register and Unresolved Questions

Severity: **S1** = silent data loss in backups; **S2** = incorrect result or availability
loss; **S3** = operational or performance cost; **S4** = maintenance.

"Unresolved" means the design currently does not have a proven answer.

---

## 1. Open decisions (must be resolved before the affected phase)

| # | Question | Blocks | Owner decision needed |
|---|---|---|---|
| Q1 | Does the product require incremental continuity across **unclean power loss**, or is "full backup after unclean shutdown" acceptable? | Phase 7 backend choice, durability design | **yes** — this single answer determines whether a custom kernel component is ever justifiable |
| Q2 | Is a snapshot provider mandatory, or is "ranges without a snapshot" a supported degraded mode? | API shape, CLI semantics | yes |
| Q3 | Which identity source is authoritative per platform (WWN vs. serial vs. user-supplied)? | Phase 1 device model | yes |
| Q4 | Retention default: bytes-first (`max_history_bytes`) or generations-first? | Phase 4/5 | yes — recommend bytes-first |
| Q5 | Is consumer authorization in scope for v1 (multiple tenants vs. single root-owned service)? | API/IPC | yes |
| Q6 | Hash for the history chain: SHA-256 (vendored) vs. BLAKE2s (vendored) vs. CRC64 only? | Phase 5 format freeze | yes |
| Q7 | License. | repo publication | yes |

---

## 2. Technical risks

### R1 — Boundary/snapshot off-by-one (S1) — **unresolved until proven**

The failure described in [consistency-model.md](consistency-model.md) §4: recording the
boundary as the generation *before* the snapshot instead of the one active *at* the
snapshot. Produces silent false negatives, passes all functional tests, and is invisible
until a restore fails.

*Mitigation:* single API entry point (`beginBackup`) that performs seal+snapshot+boundary
capture under one exclusive lock; no setters for boundary; a dedicated test
`BoundaryMatchesSnapshotGeneration` that interleaves writes with `beginBackup` and asserts
the oracle union equals the returned ranges; a code comment referencing the proof.

*Residual:* kernel-level ordering between `dmsetup suspend` and the era checkpoint must be
verified on real hardware (Phase 7).

### R2 — Unclean-shutdown window loses dirty bits (S1) — **unresolved, policy mitigates**

Without a metadata-first durable write, a power failure loses the unsealed tail.

*Mitigation:* `clean_shutdown` token + mandatory invalidation (architecture C4). Trades
correctness for availability, which is the right trade and must be stated to users.

*Residual:* an unclean shutdown of the *whole host* after a clean engine stop but before
the kernel flushes metadata is still a loss window if the backend claims
`durable_before_ack`. Only a measured, tested capability flag may claim that; default false.

### R3 — `dm-era` metadata growth is unbounded (S2)

Era array grows per era and has no documented reclaim path. See
[linux-backend.md](linux-backend.md) §3.

*Mitigation:* bounded ring usage + mandatory drain + scheduled re-baseline; documented as
an operational constraint.

*Residual:* if re-baseline frequency is unacceptable, D2 justifies a custom target.

### R4 — Userspace cannot query `dm-era` live (S2)

Metadata-snapshot + offline dump is slow and racy with concurrent checkpoints (a dump of a
snapshot is consistent, but the snapshot must be taken and then parsed).

*Mitigation:* measure; if boundary establishment exceeds ~1 s, fall to D1.

### R5 — State explosion in the lock protocol (S1/S3)

Shared/exclusive rotation is simple on paper; the risk is the "seal under exclusive lock"
version stalling writers for an `fdatasync`, or a later "optimization" that releases the
lock before publishing, exposing a sealed-but-invisible generation to sessions.

*Mitigation:* the two-phase variant in [concurrency.md](concurrency.md) §3 is documented
with its precondition (sessions wait on `last_sealed`); a state-machine invariant test
asserts that no session can ever observe `boundary > last_sealed`.

### R6 — Corrupt-but-valid metadata (S1)

A bitmap that passes CRC but represents the wrong content — e.g. restored from a snapshot,
copied from another device, or written by an older buggy version.

*Mitigation:* identity fields in every file (device_uuid, metadata_uuid, epoch,
granularity, region_count, chain hash, format version); strict rejection on mismatch; no
migration path that "best-efforts" an old format. Version bumps require an explicit
migration tool that is itself tested against a corpus of old files.

*Residual:* a deliberate rollback of the whole directory (all files from a consistent older
point) is undetectable without an external monotonic witness. Out of scope for v1; note it
in the security section rather than pretending it is covered.

### R7 — Retention policy causes surprise full backups (S2)

`max_history_age_days` and byte limits will, in production, silently invalidate a lagging
consumer unless the flag is surfaced.

*Mitigation:* invalidate-before-delete (§5.2 of bitmap-history.md); default age limit
disabled; CLI `consumer status` shows the reason and the generation at which history was
lost; the service logs a structured `consumer_invalidated` event with a stable reason code.

### R8 — Snapshot space exhaustion mid-backup (S2)

`dm-snapshot` COW overflow invalidates the snapshot during a long backup on a busy device.

*Mitigation:* fail the session (never commit); expose the required COW headroom as a
configuration check at `beginBackup`; prefer `dm-thin`. Document that snapshots are not
free and that a busy device needs headroom proportional to the change rate × backup
duration.

### R9 — Performance regression from the mark path (S3)

`fetch_or` on a contended word, plus a seal that stalls writers.

*Mitigation:* measure before optimizing; per-device seals only; publish measured
tracking overhead vs. untracked baseline in `docs/performance.md`. No invented targets:
the first benchmark defines them.

### R10 — Multi-device interference (S3)

One device's GC or checkpoint storm starving another's writes (shared thread pool, shared
metadata filesystem).

*Mitigation:* per-device serialized maintenance, bounded concurrency across devices, and a
metadata filesystem sizing guideline.

### R11 — Kernel ABI / distribution drift (S3)

DM message names, era behaviour, or metadata tool formats change across kernels;
`thin-provisioning-tools` output format is not a stable API.

*Mitigation:* parse defensively, pin a tested range, capability probe at startup (step 8
of linux-backend.md §7), and treat parse failure as `tracking_unsupported`, never as
"empty change set".

### R12 — Simulator/real divergence (S2)

The simulator is the thing all tests exercise; the kernel path will differ in exactly the
ways that matter (flush ordering, bio splitting, partial writes, discard).

*Mitigation:* both implement the same `TrackingBackend` interface; the simulator has a
`Strict` mode that models reordering and torn writes; a shared conformance suite runs
against both. Divergence in capability flags is explicit and asserted.

### R13 — Testing the crash paths themselves (S1)

Crash tests that rely on `kill -9` are flaky and non-deterministic, and can pass for the
wrong reason.

*Mitigation:* injectable crash points at named sites (see the crash matrix), plus a
`CrashPoint::failNext(N)` counter so tests are deterministic and re-runnable. Each crash
test asserts a *specific* post-recovery state, not merely "did not crash".

### R14 — Over-engineering before the core is proven (S4)

The brief's component list is large. Every module that exists before it is needed becomes a
place for a bug to hide.

*Mitigation:* the roadmap adds modules only when a phase's acceptance criteria require
them. `SnapshotCoordinator` in particular starts as a one-method interface with two
implementations (`NoSnapshot`, `DmSnapshotShell`), not a framework.

---

## 3. Assumptions currently baked in (challenge these)

| # | Assumption | If wrong |
|---|---|---|
| A1 | 64 KiB granularity is acceptable to consumers (range lists stay small) | range counts explode on random workloads; need adaptive granularity or range density caps |
| A2 | Consumers tolerate false positives | if not, the engine must read-compare blocks, which is out of scope entirely |
| A3 | A consumer's `commitBackup` is called only after its data is durable | otherwise the engine advances its cursor past unread data — must be enforced by documentation *and* by making the API shape require an explicit "data secure" call |
| A4 | One engine process owns a metadata directory | two would corrupt the WAL despite `flock` if the lock is bypassed (e.g. NFS); document local-filesystem-only |
| A5 | Metadata store lives on a reliable local filesystem with fsync semantics | on NFS/overlayfs the durability contract is unprovable; detect and refuse |
| A6 | Generation count per epoch stays under 2^63 and backup IDs under 2^63 | monotonic counters are unbounded but practically fine; still check on load |
| A7 | Sealing a few times per backup cycle is acceptable | if consumers need per-second boundaries, seal cost becomes the dominant term |
| A8 | `dm-era` metadata snapshot + offline dump is usable | if not, custom target (R3/D1) |
