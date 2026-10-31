# Linux Backend: `dm-era` Feasibility and Fallback Strategy

**Everything in §2 is a claim that must be verified on real hardware in Phase 0 before it
is allowed to influence the design.** Claims are labelled `[verified]`, `[believed]`, or
`[test]`. Nothing is `[verified]` yet — this document is the plan to get there.

---

## 1. Why not start with a kernel module

A block-level changer has three viable homes for the tracking bit, in increasing cost:

1. **Above the block layer** (userspace reading `/proc`, ioctl polling, inotify, fs-level
   hooks). Not viable: cannot see writes to the block device, races with the writer, no
   durability relationship. Excluded.
2. **Inside an existing DM target** (`dm-era`). Zero kernel code, inherits a tested
   write path, but constrained by that target's data model.
3. **A custom DM target**. Full control, full responsibility: bio splitting, flush/FUA,
   discard, error handling, metadata device protocol, kernel ABI, and years of bugs.

The correct engineering order is 2 → measure → maybe 3. Writing a target first is how
projects die.

---

## 2. `dm-era` — what it provides

`[believed]` unless marked otherwise.

- A DM target that wraps an origin device and a metadata device, with parameters
  `<metadata dev> <origin dev> <block size> <#blocks> <current era>`.
- A per-era **write set**: bitset of blocks written during the current era.
- An **era array**: persistent per-era bitsets (`era_array[era][block]`), i.e. whether a
  block was written during era *e*.
- Control messages: `checkpoint` (close the current era, advance), `invalidate` (mark a
  range as needing copy in the current era), metadata **snapshot** control
  (`take`/`get`/`restore`/`drop` of a metadata snapshot), and metadata usage queries.
- Userspace access is **not** a live ioctl returning a bitmap. The supported pattern for
  reading history is: take a metadata snapshot, then run `thin-provisioning-tools`
  (`era_dump`, `era_check`, `era_restore`, `era_invalidate`) against that snapshot
  offline. `[test]` — confirm there is no in-kernel query path we are missing.
- The metadata device holds the era array; its size grows with
  `(number of eras) × (number of blocks) / 8`. `[test]`

### Consequence for BlockChronicle

`dm-era` **does** persist per-era change information that survives reboot, and it **does**
answer "which blocks were written in era *e*". That is genuinely useful. It is **not** the
same as "give me the union of eras 11..20 for consumer A" — that must be built by BlockChronicle.

---

## 3. The scaling problem (decisive)

Metadata growth per era = `regions / 8` bytes.

| Device | Block size | Regions | Per-era metadata | 64 eras | 256 eras | 1000 eras |
|---|---:|---:|---:|---:|---:|---:|
| 1 TiB | 64 KiB | 16.78 M | 2.00 MiB | 128 MiB | 512 MiB | 2.00 GiB |
| 1 TiB | 1 MiB | 1.05 M | 128 KiB | 8 MiB | 32 MiB | 128 MiB |
| 1 TiB | 4 MiB | 262 k | 32 KiB | 2 MiB | 8 MiB | 32 MiB |
| 10 TB | 64 KiB | 167.8 M | 20.0 MiB | 1.28 GiB | 5.1 GiB | 20.0 GiB |
| 10 TB | 4 MiB | 2.62 M | 320 KiB | 20 MiB | 80 MiB | 320 MiB |

Two conclusions:

1. **64 KiB granularity × deep history is unaffordable in `dm-era`'s model.** The
   specified default (64 KiB, up to 1000 generations) does not fit. Either the history
   depth per era-array must be small (dozens), or the block size must be coarser, or the
   delta history must live in BlockChronicle's own store (which is the design chosen
   in [architecture.md](architecture.md) §3).
2. **There is no "drop era" operation.** `[test]` The era array only grows. If true, a
   device checkpointed every hour for a year has 8760 eras' worth of metadata with no
   reclaim path — a hard limit on `dm-era` as a long-lived tracker, independent of
   granularity.

### The role `dm-era` can play

Use it as a **bounded ring of recent history**, not as the archive:

```
dm-era (64 KiB, ~32–64 eras)         BlockChronicle userspace
---------------------------          -----------------------
live dirty tracking                  drains eras on a timer
seal = checkpoint message            persists D_g in its own sparse format
survives reboot                      owns 1000-generation history
bounded metadata                     unbounded, compressed, GC'd
```

This is exactly the split in [architecture.md](architecture.md) §3, and it makes `dm-era`
viable: BlockChronicle drains and persists, so era count stays bounded. It does not solve
metadata reclamation inside `dm-era`, so the **drain interval must be much shorter than
the era capacity** and the design must tolerate a metadata device that grows until a
re-baseline is scheduled. Plan for a documented annual (or size-triggered) re-baseline
that creates a fresh `dm-era` mapping, which is a full-backup event.

---

## 4. Gap analysis against BlockChronicle requirements

| Requirement | `dm-era` | Gap / work |
|---|---|---|
| 64 KiB granularity | supported (param) | metadata cost, §3 |
| Track writes durably across reboot | yes | — |
| Per-era change sets | yes | — |
| Union over N eras | no direct API | BlockChronicle computes it; drain to own store |
| Live query from userspace | no (metadata snapshot + offline dump) | **major**: drainer + parser, or a custom ioctl |
| Bounded metadata lifetime | no (era array grows) | re-baseline policy or custom target |
| Durable-before-ack per write | `[test]` — depends on metadata device cache/FUA behaviour | must be measured; assume **no** |
| Indicate tracking validity after unclean power | `[test]` — likely no strong signal | BlockChronicle `clean_shutdown` token |
| Multiple independent consumers | n/a (not its job) | BlockChronicle |
| Per-consumer cursors | n/a | BlockChronicle |
| Snapshot coordination | n/a (orthogonal) | BlockChronicle + dm-snapshot/thin |
| Discard handling | `[test]` | define: discard marks dirty (conservative) |
| Error/partial write handling | inherits DM | verify |
| Kernel ABI stability | stable DM messages | low risk |

The two rows that decide the architecture: **live query** and **bounded metadata**. Both
are solvable in userspace, which is why `dm-era` is a legitimate Phase 7 target rather
than a dead end.

---

## 5. Decision criteria for a custom target

Build a custom DM target only if at least one of these is demonstrated with measurements,
recorded in `docs/linux-backend.md` as a dated finding:

- **D1 — Query latency.** Drain-from-metadata-snapshot takes long enough that a backup
  cannot get a boundary within its SLA (target: boundary establishment < 1 s including
  snapshot creation). Measure on a 1 TiB and a 10 TB origin.
- **D2 — Metadata growth.** Era-array growth cannot be bounded by the drain interval
  because checkpoints must be frequent (e.g. per-minute boundaries), so the metadata
  device becomes the limiting resource.
- **D3 — Durability.** No configuration of `dm-era` + metadata device yields a
  durable-before-ack property, *and* the product requires incremental continuity across
  power loss rather than "full backup after unclean shutdown". (Note: requirement D3 is a
  product decision, not a technical one. If the product accepts invalidation-on-unclean-
  shutdown — which the brief's rule 10 permits — D3 does not justify a kernel module.)
- **D4 — Granularity.** The required granularity is finer than `dm-era` can sustain, or
  the origin's block size makes the shared write set unusable.

If none holds, **do not write a kernel module.** Document the measurement and move on.

### Custom target design sketch (only if justified)

Deliberately minimal, and the same shape as the simulator so the userspace side does not
change:

```
target: bc-chronicle <metadata_dev> <origin_dev> <granularity_sectors> <ring_slots>

in-memory:
  struct bc_ring {
      struct bc_delta slots[RING];   // RING = 32 or 64
      u32 head, tail;
      u64 active_gen;
      spinlock_t lock;               // protects head/tail + slot bitmap only
  };

write path (map callback):
  first = bio->bi_iter.bi_sector / granularity_sectors
  last  = (bi_sector + bi_size/512 - 1) / granularity_sectors
  set bits in slots[head] bitmap            // test_and_set_bit, no lock needed per bit
  clone bio, submit to origin               // do not delay the data

control messages (userspace):
  "seal"        -> move head, append delta header to metadata dev (REQ_FUA), wake poller
  "read <gen>"  -> copy delta bits into a userspace buffer (ioctl or DM message reply)
  "invalidate"  -> mark range dirty in active slot
```

Hard parts that must be designed up front (this is why the bar for D1–D4 is high):

- Metadata durability ordering vs. the data bio. If `seal`'s metadata write is the only
  durability point, then `durable_before_ack = false` and the same
  invalidate-on-unclean-shutdown rule applies — which means the module buys *query
  latency*, not durability. Be honest about this in the design doc.
- Ring overflow while userspace is not draining: the correct behaviour is to **stop
  sealing** and, if the ring is full, mark the device invalid (never drop a delta).
- `REQ_PREFLUSH`/`REQ_FUA` propagation and `discard`/`write_zeroes` handling.
- Module unload with dirty state; device removal (`-EREMOVE`) mid-flight.
- Testing without a kernel CI: `dm-flakey`-style fault injection plus a userspace
  `dmsetup` test harness.

---

## 6. Snapshot integration ordering (Linux-specific)

For `dm-snapshot` / thin:

```
1. dmsetup suspend <origin-mapped-dev>          # no I/O can complete or start
2. message to era target: "checkpoint"          # seal generation G  (now atomic vs writes)
3. dmsetup create <snap> --table "snapshot ..."  # snapshot of the suspended device
4. dmsetup resume <origin-mapped-dev>
5. record session boundary = G
```

- Suspend time must be low single-digit milliseconds. Measure per filesystem load
  (`suspend` waits for in-flight I/O to drain). This is the concrete number that decides
  whether the design is operable at all.
- The snapshot must be created **after** the checkpoint, per
  [consistency-model.md](consistency-model.md) §4.
- With `dm-thin`, the equivalent is `dmsetup message <thin-pool> "create_snap ..."`, which
  is cheaper, but still needs the pause for a *clean* boundary. For `dm-thin` an
  alternative that avoids suspend is available when the era target's active delta can be
  observed before the thin snapshot is taken — verify, do not assume.
- On an unclean shutdown, snapshots may be `Incomplete` (`dm-snapshot` merge/status) and
  must be discarded; the session is failed and the consumer cursor untouched.

Snapshot exhaustion behaviour must be explicit: `dm-snapshot` sets
`Invalid`/`Overflow`; the engine must fail the session, never commit the consumer.

---

## 7. Runtime capability detection (not distribution assumptions)

```
1. /sys/module/dm_era exists?  else modprobe dm_era (report failure verbatim, no fallback)
2. dmsetup targets | grep -qx era
3. kernel version >= tested floor (record min per distribution)
4. metadata device exists, writable, and is not the origin itself
5. origin is not already claimed by another era target (dmsetup table check)
6. snapshot provider: dmsetup targets | grep -qx snapshot  (or thin-pool present)
7. permissions: /dev/mapper/control openable, appropriate CAP_SYS_ADMIN
8. attempt a scratch 16 MiB loop-backed era create/checkpoint/dump/destroy cycle
   -> this is the only check that proves the whole path works
```

Step 8 is the real capability test; steps 1–7 are fast pre-checks that produce better
error messages. Cache the result with a kernel version + module hash key, and record it in
the manifest (`backend_caps`) so post-hoc debugging is possible.

Compatibility matrix (fill in with real test results, never with vendor claims):

| Distribution | Kernel | `dm-era` present | 64 KiB viable | Snapshot | Verified on | Notes |
|---|---|---|---|---|---|---|
| RHEL 9.x | | | | | | |
| CentOS Stream 9/10 | | | | | | |
| Debian 12/13 | | | | | | |
| Ubuntu 22.04/24.04 | | | | | | |
| Fedora (current) | | | | | | |

An empty cell means "not tested", and the product must refuse to claim support for an
untested combination unless the user opts into an explicit `--allow-untested-backend`
with a warning.

---

## 8. Fallback ladder (in order, no skipping)

1. **Default distribution `dm-era`, drained by BlockChronicle.** Covers most cases.
2. **Shorter drain interval / coarser granularity per device** — configuration only, no
   code. Document the trade (granularity × history depth).
3. **No kernel tracking: simulator only**, for development, CI, and single-host
   validation. Must be reported as `tracking_unsupported` for real devices.
4. **Custom target** — only after D1–D4 evidence.
5. **Refuse.** If none applies, the engine reports that incremental backup is unavailable
   for the device. It never emits a partial range list with a warning in a log file.

Row 5 is a product feature, not a failure.
