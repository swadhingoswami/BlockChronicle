# Persistent Metadata, WAL, and Crash Recovery

Rules that apply to every file described here:

- **Explicit serialization.** Little-endian fixed-width fields, written field by field.
  No `memcpy` of structs, no `#pragma pack`, no `reinterpret_cast` of buffers.
- **Every file is self-describing**: magic, format version, header length, payload length,
  CRC32C, and the identity fields needed to reject a file that belongs elsewhere.
- **Every file is validated on load**, including cross-checks against the superblock.
- **Nothing is repaired silently.** Validation failure ⇒ a defined degradation path.

---

## 1. Directory layout

```
/var/lib/blockchronicle/
├── LOCK                          # flock(LOCK_EX|LOCK_NB); single-writer enforcement
├── registry/
│   └── devices.db                # device_uuid -> relative path, identity summary
└── devices/<device-uuid>/
    ├── manifest.bin              # A/B superblock, 3 × 4 KiB
    ├── wal/
    │   ├── segment-00000001.wal
    │   └── segment-00000002.wal
    ├── history/
    │   ├── gen-00000010.delta
    │   ├── cum-00000016.bitmap
    │   └── snapshots/<snapshot-uuid>/{meta.rec,bitmap.bin}
    └── consumers/
        └── consumer-<escaped-id>.rec
```

Deviations from the brief's suggested layout, and why:

- `active.bitmap` / `frozen.bitmap` as *files* are removed. The active generation's delta
  lives in the backend (kernel ring or simulator memory) and is only written on seal, as
  `history/gen-<g>.delta`. Writing an "active bitmap" file would require an fsync per
  write or would be permanently stale — worse than useless, because a stale file looks
  authoritative after a crash.
- `checkpoints/` is collapsed into `history/cum-*` plus the superblock. Checkpoints are
  *the* manifest state; a separate directory invites disagreement between the two.
- `snapshots/snapshot-000010.meta` is replaced by `snapshots/<uuid>/` so that identity and
  lifetime are explicit and deletion is one `unlinkat` of a directory, not a guess from a
  generation number (multiple analysis snapshots may share a generation).
- Consumer files are one file per consumer (not one big table) so a corrupt consumer
  record affects exactly one consumer. Consumer ID is escaped (percent-encoding of any
  byte outside `[A-Za-z0-9._-]`, `%` escaped first) and the length is capped at 128 bytes.
  The escaping must be injective and reversible; there is a unit test with `../`, NUL,
  `/`, `%`, and a 4 KiB ID.
- `devices.db` is a **cache**, not the source of truth. The truth is each device's
  `manifest.bin`. If `devices.db` disagrees or is corrupt, it is rebuilt by scanning
  `devices/*/manifest.bin`. This removes an entire failure mode.

---

## 2. Superblock (`manifest.bin`)

Three 4 KiB slots: `A` at 0, `B` at 4096, reserved at 8192. Writers alternate A/B and
pick the highest `sequence` that validates. This gives atomic update with a single fsync
and no rename dependency, which matters because the superblock is rewritten often.

| Field | Type | Notes |
|---|---|---|
| `magic` | 8 bytes | `"BCLGBK\0\1"` |
| `format_version` | u32 | 1 |
| `header_len` | u32 | 256 |
| `sequence` | u64 | monotonic, slot selection key |
| `device_uuid` | 16 bytes | stable identity |
| `metadata_uuid` | 16 bytes | random at creation; detects directory copy/rollback |
| `device_epoch` | u64 | bumped on re-baseline |
| `capacity_bytes` | u64 | identity check |
| `granularity_bytes` | u32 | 65536 |
| `region_count` | u32 | derived, stored for validation |
| `active_generation` | u64 | current |
| `last_sealed_generation` | u64 | visible to sessions |
| `oldest_retained_generation` | u64 | GC floor result |
| `cum_interval` | u32 | `K` = 16 |
| `last_cum_generation` | u64 | |
| `wal_first_seq` | u64 | replay starts here |
| `wal_last_seq` | u64 | highest durable record |
| `wal_first_segment` / `wal_last_segment` | u64 | |
| `history_chain_hash` | 32 bytes | `link_G` of the last sealed generation |
| `next_backup_id_flags` | u64 | reserved / bitfield |
| `tracking_valid` | u8 | 0/1 |
| `clean_shutdown` | u8 | 0 = unclean, 1 = set on graceful stop |
| `backend_id` | u16 | enum: 1 simulator, 2 dm-era, 3 custom |
| `backend_caps` | u16 | bitfield, **recorded at seal time** |
| `created_at_ns` | u64 | |
| `reserved` | … | zero-filled |
| `header_crc32c` | u32 | over header minus this field |
| `slot_crc32c` | u32 | over the full 4 KiB slot |

Two independent CRCs is intentional: `header_crc32c` catches a torn header, and
`slot_crc32c` catches a torn sector write of the whole slot including zero padding.

Selection rule: among slots whose `magic`, `format_version`, and both CRCs validate, take
the highest `sequence`. Ties are impossible unless a writer bug exists; treat a tie as
`Corrupt`.

`backend_caps` is stored because capability decisions must be reproducible after reboot.
If the backend's capability set *shrinks* between boots, that is an identity-level change:
invalidate. Growing is allowed (log it).

---

## 3. Delta and cumulative bitmap files

Common header (64 bytes fixed + variable):

| Field | Type |
|---|---|
| `magic` | 8 bytes `"BCLGBMP\0"` |
| `format_version` | u32 |
| `kind` | u8 (1 delta, 2 cumulative) |
| `encoding` | u8 (0 dense, 1 sparse) |
| `flags` | u16 (reserved, must be 0) |
| `device_uuid` | 16 bytes |
| `metadata_uuid` | 16 bytes |
| `device_epoch` | u64 |
| `generation` | u64 |
| `baseline_generation` | u64 (0 for deltas) |
| `granularity_bytes` | u32 |
| `region_count` | u32 |
| `payload_len` | u64 |
| `prev_chain_hash` | 32 bytes |
| `payload_crc32c` | u32 |
| `header_crc32c` | u32 |

Loading validates: magic/version, `device_uuid`, `metadata_uuid`, `device_epoch`,
`granularity`, `region_count`, payload length equal to the encoding's implied size,
`payload_crc32c`, then the chain link. A file failing any check is quarantined
(renamed to `*.quarantine-<n>`).

Quarantine rather than delete: it preserves evidence for postmortem and prevents a
crash-loop where a file is repeatedly picked up.

---

## 4. WAL

### 4.1 Segmentation

Fixed-size segments (default 8 MiB) named `segment-%08d.wal`. A record never spans
segments; a record that does not fit starts the next segment. Segment creation is
`open(O_CREAT|O_EXCL)`, records appended with `write`, durability via `fdatasync`.

### 4.2 Record framing

| Field | Type | Notes |
|---|---|---|
| `magic` | u32 | `0x57414C52` (`WALR`) |
| `record_len` | u32 | total including header and CRC |
| `seq` | u64 | strictly increasing, per device |
| `type` | u16 | see below |
| `flags` | u16 | bit0 = tombstone (logically deleted) |
| `generation` | u64 | 0 if not generation-scoped |
| `payload_len` | u32 | |
| `payload` | bytes | type-specific, explicit LE encoding |
| `record_crc32c` | u32 | over header+payload |

`record_len` is validated to be `>= sizeof(header)+4` and `<= segment_size`. A record is
accepted only if `magic`, lengths, and CRC all validate **and** `seq == expected_seq`
(contiguous; no gaps allowed within a segment). The first failure stops replay.

### 4.3 Record types

Control plane only (see architecture C3):

| Type | Payload | Purpose |
|---|---|---|
| `SEAL` | gen, chain hash, delta file ref, region count, dirty count | makes `D_g` visible |
| `CONSUMER_REGISTER` | consumer id, cursor gen 0, epoch | baseline pending |
| `CONSUMER_REMOVE` | consumer id | releases floor contribution |
| `CONSUMER_COMMIT` | consumer id, epoch, boundary gen, backup id, chain hash of D_boundary | advances cursor |
| `SESSION_OPEN` | session id, consumer, boundary, snapshot id, pid, boot id | crash reclamation |
| `SESSION_ABORT` | session id, reason | |
| `TRACKING_INVALIDATE` | epoch, scope (device / generation range), reason | |
| `REBASELINE` | old epoch, new epoch | |
| `SNAPSHOT_CREATE` / `SNAPSHOT_RELEASE` | session, snapshot provider id, device | snapshot lifetime |
| `DEVICE_ATTACH` / `DEVICE_DETACH` | identity hash, clean flag | lifecycle |
| `GC_PLAN` | target epoch/gen, affected consumers | intent before delete |
| `GC_DONE` | plan seq | |
| `CHECKPOINT` | sequence covered, superblock sequence | replay shortcut marker |
| `MARK_RANGE` | gen, offset, length | **simulator + crash tests only**; rejected when `backend_id != simulator` |

Rejecting `MARK_RANGE` for real backends is deliberate: it makes it impossible for a
misconfigured build to believe userspace is tracking physical writes.

### 4.4 Why a plain sequential log is enough

No ABI stability, no concurrent writers, no reader/writer coupling: the WAL has exactly
one writer (the engine, under the device lock for control records) and is replayed once at
startup. It does not need a ring buffer, checkpoints-inside-the-WAL, or group commit until
a benchmark proves otherwise. It must only be *correct*.

Group commit is however trivial to add later: the mutex is already held around
`append+fsync`, so batching means releasing the lock after `write` and having a single
flusher thread `fdatasync`. Note it as a Phase 6 optimization; do not build it now.

---

## 5. Checkpoint protocol

```
1. exclusive device lock
2. snapshot in-memory state (shared_ptr<const Bitmap> for the delta being sealed)
3. write delta to history/gen-<g>.delta.tmp ; fsync ; rename ; fsync(dir)
4. if (g % K == 0) write cum-<g>.bitmap.tmp ; fsync ; rename ; fsync(dir)
5. append CHECKPOINT record ; fsync
6. write superblock slot (seq+1) with new active/last_sealed/wal_first_seq ; fdatasync
7. unlink wal segments whose last durable seq < wal_first_seq
8. fsync(wal dir)
9. release exclusive lock
```

Crash table:

| Crash point | On-disk state | Recovery action |
|---|---|---|
| before 3 | old superblock, WAL has no CHECKPOINT | replay from `wal_first_seq`; leftover `.tmp` deleted |
| during 3 | partial `.tmp` | delete `.tmp`; replay from `wal_first_seq` |
| after 3, before 6 | orphan delta/cum files | ignored: not referenced by superblock or `SEAL`/`CHECKPOINT` records. GC deletes them by rule "no reference from replay state". |
| during 6 | one slot new, one old | highest valid `sequence` wins. If the newer slot is torn (CRC fail), the older slot is used and the WAL replay re-derives the missing state, because the WAL was not truncated yet. |
| after 6, before 7 | stray segments | replay starts at `wal_first_seq`; stray segments are deleted |
| after 7 | normal | — |

The invariant making this work: **the superblock is written last and is the sole authority
for WAL boundaries; deletion only ever removes data that is already provably redundant.**

---

## 6. Recovery algorithm

```
open LOCK (flock, exclusive, non-blocking)      -> fail fast if another engine runs
scan devices/*/manifest.bin
for each device:
    read both slots, choose highest valid sequence
      both invalid     -> QUARANTINE device (rename dir to <uuid>.quarantine-<ts>),
                          consumers all full_backup_required, notify operator, continue
    verify identity: metadata_uuid, device_uuid, capacity, granularity, backend_caps
      mismatch         -> INVALIDATE (do not delete)
    load history: newest cumulative <= last_sealed, then delta chain up to last_sealed
      gap/CRC fail     -> invalidate consumers whose (cursor, boundary] touches the hole
    replay WAL from wal_first_seq to EOF
      clean EOF        -> normal
      partial tail     -> truncate at last valid seq (record was never acked)
      CRC failure mid-log with more valid records after it -> STOP and INVALIDATE
                          (cannot prove nothing was skipped)
      seq gap          -> INVALIDATE
    if clean_shutdown != 1 -> INVALIDATE (devices with durable_before_ack == false)
    reclaim stale SESSION_OPEN records (pid/boot id mismatch, older than grace period)
    recompute floor, run GC if configured
    write manifest with clean_shutdown = 0      (armed)
```

On graceful stop: replay-complete → set `clean_shutdown = 1`, `fdatasync`, release lock.

### The partial-tail decision

A partial or CRC-failed **last** record means the writer crashed mid-append. Since an
acknowledged control operation implies a completed `fdatasync`, such a record was never
observed by any client, so truncating it cannot lose an acknowledged fact. Truncation is
safe *only* for the final record of the final segment. A malformed record followed by
valid records means the log was corrupted, not torn — and then nothing can be proven, so
invalidate.

This asymmetry must be a comment in the code and a named test
(`WalTruncatesOnlyTheFinalPartialRecord`), because "just truncate at the first bad byte"
is the tempting wrong implementation.

### Recovery states reported to operators

| State | Meaning | Consumer impact |
|---|---|---|
| `Clean` | resumed, history intact | none |
| `Replayed` | resumed after clean-crash, WAL replayed | none |
| `Degraded` | some history lost, affected consumers invalidated | those consumers need full backup |
| `Invalidated` | device tracking unusable, re-baseline required | all consumers need full backup |
| `Quarantined` | metadata unreadable | all consumers need full backup; manual intervention |

Each is a distinct CLI exit status and a distinct log event. Operators must never have to
guess which happened.

---

## 7. Crash-test matrix (Phase 5 acceptance)

Each row is a test that terminates the process (or the simulator) at a specific point and
asserts the recovery outcome. Implemented with an injectable `ICrashPoint` callback, not
`kill -9` timing.

| # | Crash point | Expected |
|---|---|---|
| 1 | after mark, before data write | region reported dirty (false positive OK) |
| 2 | after data write, before ack | region reported dirty |
| 3 | mid delta `.tmp` write | old state intact, `.tmp` cleaned |
| 4 | after delta rename, before superblock | replay re-derives; no duplicate seals |
| 5 | superblock torn in one slot | other slot wins |
| 6 | both superblock slots torn | quarantined, all consumers invalidated |
| 7 | after `SEAL` fsync, before snapshot create | session fails cleanly; generation sealed but unreferenced; GC reclaims |
| 8 | after `CONSUMER_COMMIT` fsync, before reply | consumer's next `commitBackup` with same backupId is idempotent (returns Ok) |
| 9 | before `CONSUMER_COMMIT` fsync | cursor unchanged; retry re-reads same boundary |
| 10 | during GC after `GC_PLAN`, before delete | replay honours plan; flags set; deletion resumes |
| 11 | during WAL segment rotation | new segment absent ⇒ old segment is authoritative; record retried |
| 12 | after truncating WAL, before dir fsync | stray segments ignored by `wal_first_seq` |
| 13 | unclean shutdown with kernel backend | device invalidated, all consumers `full_backup_required` |
| 14 | device swapped under same path | identity mismatch detected at attach |
| 15 | metadata dir copied from another host | `metadata_uuid` mismatch ⇒ refuse to open |

Idempotency of `commitBackup` (row 8) deserves emphasis: the reply may be lost after the
fsync, so the consumer will retry. Making commit idempotent by `(consumer, backupId)` is
cheaper and safer than any two-phase protocol. Same for `SEAL` keyed by generation.

---

## 8. Security-relevant details

- `LOCK` via `flock` prevents two engines writing one directory. It does not prevent a
  malicious process; that requires filesystem permissions (root:root, 0700) and is
  documented, not pretended.
- All paths are constructed, never concatenated from user input. Consumer IDs are
  percent-escaped and length-capped; device paths from the registry are opened with
  `O_NOFOLLOW` where applicable.
- `devices.db` entries are validated against the filesystem (`realpath` containment check)
  before use; a `../` escape in the registry is a hard error.
- Metadata files are opened with `O_RDONLY` for validation before any mutation, and all
  writes go through `O_CREAT|O_EXCL` temp names in the same directory.
- Sizes from headers are validated against the file size *before* allocation; a 4 KiB
  header must never cause a 4 GiB `resize`.
- No metadata field is trusted for arithmetic without range checks (`region_count *
  granularity` overflow).
