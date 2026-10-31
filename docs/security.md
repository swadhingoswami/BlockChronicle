# Security and Reliability Notes

Scope: the userspace engine and its metadata. A kernel component, if one is ever built,
adds its own attack surface and is out of scope here.

---

## 1. Assets

| Asset | Why it matters |
|---|---|
| Metadata directory `/var/lib/blockchronicle` | Losing it costs a full backup, not data. Corrupting it silently could cost a backup's correctness. |
| Device nodes the service can open | Read/write access to user data. |
| Consumer cursors | Determine how much data the next backup covers. A wrong cursor means an incomplete backup that reports success. |

## 2. Trust boundaries

Everything the engine reads is untrusted input: the registry file, every metadata file,
config files, and the identity fields inside them. Device paths come from the operator,
not from an attacker, but they are still validated.

The engine does **not** implement authentication or authorization. It is a local
root-owned service. If a remote API is added later, every consumer identity must become
an authenticated principal; the current design assumes a trusted caller.

## 3. What is implemented

| Control | Where |
|---|---|
| Explicit little-endian serialization, no `memcpy` of structs | `codec.h` |
| CRC32C on every object; header and payload checksums are independent | `crc32c.h`, `object_header.h` |
| Identity validation: device UUID + metadata UUID + epoch on every file | `superblock.cpp`, `delta_file.cpp` |
| A metadata directory copied from another host is refused, not adopted | `Superblock.RejectsForeignIdentity` |
| Torn superblock slot falls back to the other slot; both torn ⇒ `Corrupt` | `Superblock.BothSlotsCorruptIsACorruptResult` |
| Truncated WAL tail discarded; mid-log corruption ⇒ refusal, never "resume with a gap" | `Wal.MidLogCorruptionIsRefused` |
| Size fields validated against the file length before allocation | `deltaRead`, `decodeSlot` |
| Device ids restricted to `[A-Za-z0-9._-]`, ≤ 64 chars | `Engine::validDeviceId` |
| Consumer ids: ≤ 128 chars, control characters rejected | `ConsumerManager::validateId` |
| Config: unknown sections/keys and malformed values are errors | `parseConfig` |
| Read-only `verify` that writes nothing | `verifyDevice` |
| No path is built from unvalidated input; registry paths are used only after the id check | `Engine` |

## 4. Known gaps — state them, do not paper over them

1. **No cryptographic integrity.** CRC32C detects corruption, not tampering. An attacker
   with write access to the metadata directory can rewrite history and recompute every
   checksum. The design called for a cross-generation chain hash; it is not implemented
   (`risks.md` Q6). Consequence: a *consistent* rollback of the whole directory is
   undetectable.
2. **No authentication between consumers.** Any process that can open the socket/CLI can
   act as any consumer. v1 assumes a single trusted operator.
3. **`fsync` semantics are not verified.** The crash tests prove ordering, not durability
   (see `implementation-status.md`). On macOS the difference is measurable; on Linux it
   needs power-cut testing.
4. **The simulator's device identity is derived from the device id** (FNV-1a). It is
   reproducible, not validated against hardware. Do not mistake it for WWID checking.
5. **Resource exhaustion is only partially bounded.** `max_consumers` is enforced,
   range queries take a limit, WAL payloads are capped at 1 MiB. There is no global cap on
   total metadata bytes or on concurrent sessions beyond one per consumer.
6. **`flock` is specified but not yet implemented** in `PersistentDevice`. Two processes
   pointing at one metadata directory will corrupt it. Do not run the CLI and a service
   against the same root until this lands. (Tracked below.)
7. **`verify` can race a live writer.** It reports what it sees; a torn tail during a live
   append is normal and is reported as `torn-tail: yes`, not as an error.

## 5. Required before any multi-tenant or remote deployment

- Authenticate and authorize consumer identities.
- Replace ad-hoc identity derivation with measured hardware identity (WWID + a
  BlockChronicle-written epoch marker in the backend's metadata device).
- Implement the generation chain hash and decide whether it needs to be cryptographic.
- Implement `flock` and a single-writer guard with a clear operator-facing error.
- Add an audit trail for destructive operations (`device remove`, `gc`).

## 6. Review checklist for changes

1. Does any new field come from a file without a bounds check?
2. Does any new persistent object lack a magic, version, identity and checksum?
3. Does a failure path delete anything before the corresponding state is durable?
4. Can a malformed file cause an allocation larger than the file itself?
5. Does a new CLI option accept a path or id without validation?
6. Does a new log line include unbounded user-controlled data (log injection)?
