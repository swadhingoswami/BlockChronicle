# dm-era Feasibility Report

**Status: NOT RUN.** This file is a template. Nothing in it has been measured.
The scripts in `tools/dm-era/` produce the numbers; copy them here with the kernel
version and hardware they came from.

> A report with invented numbers is worse than an empty one. Leave the cells blank
> until somebody runs the experiments.

---

## 1. Environment

| Item | Value |
|---|---|
| Date | |
| Distribution / version | |
| Kernel | |
| `dm_era` built-in or module | |
| Origin device | |
| Block size used | |
| Metadata device | |
| Hardware | |

Probe output (`tools/dm-era/probe.sh`):

```
paste output here
```

## 2. Measurements

`tools/dm-era/experiments.sh`:

```
paste output here
```

## 3. Answers the architecture needs

| Question | Where it matters | Answer |
|---|---|---|
| Does the era array grow without bound? | Retention design (§3 of `linux-backend.md`) | |
| Bytes of metadata per era at 64 KiB blocks | Feasibility of 64 KiB granularity | |
| Is there any in-kernel or ioctl path that returns the write set without an offline dump? | Query latency, D1 | |
| How long does `take/get_metadata_snapshot` + `era_dump` take at 1 TiB scale? | D1 | |
| Does `dmsetup suspend` + snapshot + resume fit an acceptable window? | Operability | |
| After an unclean reboot, are the last N checkpoints present? | Durability claim | |
| Can `durable_before_ack` ever be true? | Which capability flag we may set | |
| What happens to a discard? | Whether discard marks dirty | |

## 4. Decision

Check exactly one, with the evidence that supports it:

- [ ] **dm-era is sufficient** as a bounded ring drained by BlockChronicle.
      Evidence:
- [ ] **dm-era is usable only with coarser granularity or shallower history.**
      Constraints:
- [ ] **dm-era is insufficient**; D1/D2/D3/D4 triggered. Evidence:
      (If this box is checked, `docs/linux-backend.md` §5 requires the triggering
      measurement to be recorded before any kernel work starts.)

## 5. Compatibility matrix

Fill only rows that were actually run. An untested row must stay empty: the CLI
refuses untested backends by design.

| Distribution | Kernel | era target | 64 KiB viable | Snapshot | Verified date |
|---|---|---|---|---|---|
| RHEL 9.x | | | | | |
| CentOS Stream 9/10 | | | | | |
| Debian 12/13 | | | | | |
| Ubuntu 22.04/24.04 | | | | | |
| Fedora | | | | | |
