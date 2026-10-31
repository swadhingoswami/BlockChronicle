# Packaging notes

## What exists

The userspace simulator backend has **no long-running daemon**, because there is nothing
for it to serve continuously: tracking is driven by whoever performs the writes, and the
metadata work is periodic. Shipping a `Type=simple` unit for a process that would sit in
a loop doing nothing would be theatre.

What does exist is periodic maintenance:

- `blockchronicle checkpoint` — flush the superblock for every registered device and
  rotate the journal.
- `blockchronicle gc --keep-generations N` — reclaim history no consumer still needs.

`packaging/blockchronicle-checkpoint.{service,timer}` run both every five minutes.

A `blockchronicle.service` daemon belongs to the kernel-backend phase: once a `dm-era`
(Phase 7) backend exists, something must continuously drain eras into userspace history,
so the daemon gains a real job.

## Install

```bash
sudo install -Dm0755 build/blockchronicle /usr/local/bin/blockchronicle
sudo install -Dm0644 packaging/blockchronicle-checkpoint.service /etc/systemd/system/
sudo install -Dm0644 packaging/blockchronicle-checkpoint.timer   /etc/systemd/system/
sudo install -d -m0700 -o root -g root /var/lib/blockchronicle

sudo systemctl daemon-reload
sudo systemctl enable --now blockchronicle-checkpoint.timer
```

## Configuration

`--config /etc/blockchronicle/blockchronicle.conf` or `BLOCKCHRONICLE_ROOT`:

```ini
[storage]
root = /var/lib/blockchronicle

[backup]
max_consumers = 10

[simulator]
journal_marks = true
```

`root` must be a local filesystem. `parseConfig` rejects URLs and unknown keys rather than
ignoring them: a typo in a setting should fail at start-up, not during a restore.

## Permissions

- `/var/lib/blockchronicle` must be `0700` and owned by the service account.
- Device nodes and data files must be readable/writable by that account only.
- The maintenance units run as `root` because they may need to touch device files; when
  the kernel backend lands, replace `User=root` with a dedicated account that has the
  minimum capabilities (`CAP_SYS_ADMIN` for device mapper, nothing else).

## Upgrade

There is no metadata migration yet. The format carries a version in every file header;
a future incompatible change must ship a migration tool and bump the version. Opening a
newer directory with an older binary fails with `Corrupt` instead of guessing, which is
the intended behaviour.
