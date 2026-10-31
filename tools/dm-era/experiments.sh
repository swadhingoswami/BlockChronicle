#!/usr/bin/env bash
# BlockChronicle dm-era measurements.
#
# Produces the numbers that decide the Linux backend question (D1-D4 in
# docs/linux-backend.md). Run on the target Linux host, as root, after probe.sh
# reports success.
#
#   ./experiments.sh [output.md]
#
# Results are machine-specific. Paste them into docs/dm-era-report.md with the
# kernel version and hardware; do not edit the numbers by hand.

set -uo pipefail

OUT="${1:-}"

need_root() { [ "$(id -u)" -eq 0 ]; }
if ! need_root; then
    echo "must run as root" >&2
    exit 1
fi

work="$(mktemp -d)"
dm_name="bc-exp-$$"
meta_loop=""
origin_loop=""

cleanup() {
    dmsetup remove "$dm_name" >/dev/null 2>&1 || true
    [ -n "$meta_loop" ] && losetup -d "$meta_loop" >/dev/null 2>&1 || true
    [ -n "$origin_loop" ] && losetup -d "$origin_loop" >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT

emit() { printf '%s\n' "$*"; }

now_ms() {
    # Portable millisecond clock.
    if command -v perl >/dev/null 2>&1; then
        perl -MTime::HiRes=time -e 'printf "%.0f", time()*1000'
    else
        date +%s%3N
    fi
}

ORIGIN_MB="${ORIGIN_MB:-512}"
BLOCK_SECTORS="${BLOCK_SECTORS:-128}"   # 64 KiB
ERAS="${ERAS:-64}"
META_MB="$((ORIGIN_MB / 8 + 8))"

emit "## Environment"
emit
emit "- date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
emit "- kernel: $(uname -r)"
emit "- arch: $(uname -m)"
emit "- origin size: ${ORIGIN_MB} MiB"
emit "- block size: $((BLOCK_SECTORS / 2)) KiB"
emit "- eras requested: ${ERAS}"
emit "- metadata device: ${META_MB} MiB"
emit

dd if=/dev/zero of="$work/meta.img" bs=1M count="$META_MB" status=none
dd if=/dev/zero of="$work/origin.img" bs=1M count="$ORIGIN_MB" status=none

meta_loop="$(losetup -f --show "$work/meta.img")"
origin_loop="$(losetup -f --show "$work/origin.img")"

sectors=$((ORIGIN_MB * 1024 * 1024 / 512))
blocks=$((sectors / BLOCK_SECTORS))

if ! dmsetup create "$dm_name" \
    --table "0 $sectors era $meta_loop $origin_loop $BLOCK_SECTORS $blocks 1"; then
    emit "FAILED to create era target"
    exit 1
fi

# ---------------------------------------------------------------------------
# E1: metadata growth per era
# ---------------------------------------------------------------------------
emit "## E1: metadata growth"
emit
emit "| era | metadata bytes | bytes/era |"
emit "|---:|---:|---:|"

base=""
for i in $(seq 1 "$ERAS"); do
    dmsetup message "$dm_name" 0 checkpoint >/dev/null || break
    if [ $((i % 8)) -eq 0 ] || [ "$i" -eq 1 ]; then
        size="$(stat -c %s "$work/meta.img" 2>/dev/null || stat -f %z "$work/meta.img")"
        if [ -z "$base" ]; then
            base="$size"
            per="n/a"
        else
            per=$(((size - base) / i))
        fi
        emit "| $i | $size | $per |"
    fi
done
emit

# ---------------------------------------------------------------------------
# E2: checkpoint latency
# ---------------------------------------------------------------------------
emit "## E2: checkpoint latency"
emit
total=0
count=0
for i in $(seq 1 64); do
    start="$(now_ms)"
    dmsetup message "$dm_name" 0 checkpoint >/dev/null || break
    end="$(now_ms)"
    total=$((total + (end - start)))
    count=$((count + 1))
done
if [ "$count" -gt 0 ]; then
    emit "- checkpoints: $count"
    emit "- mean: $((total / count)) ms (includes dmsetup process startup)"
else
    emit "- no checkpoints completed"
fi
emit

# ---------------------------------------------------------------------------
# E3: metadata snapshot and dump latency
# ---------------------------------------------------------------------------
emit "## E3: drain path (metadata snapshot + era_dump)"
emit
if dmsetup message "$dm_name" 0 take_metadata_snapshot 2>/dev/null; then
    start="$(now_ms)"
    if dmsetup message "$dm_name" 0 get_metadata_snapshot 2>"$work/snap.err"; then
        got="$(now_ms)"
        emit "- get_metadata_snapshot: $((got - start)) ms"
        if command -v era_dump >/dev/null 2>&1; then
            dump_start="$(now_ms)"
            era_dump "$work/meta.snap" >"$work/dump.xml" 2>"$work/dump.err" || true
            dump_end="$(now_ms)"
            if [ -s "$work/dump.xml" ]; then
                emit "- era_dump: $((dump_end - dump_start)) ms, $(wc -l <"$work/dump.xml") lines"
            else
                emit "- era_dump produced nothing: $(cat "$work/dump.err")"
            fi
        else
            emit "- era_dump not installed"
        fi
    else
        emit "- get_metadata_snapshot failed: $(cat "$work/snap.err")"
    fi
else
    emit "- take_metadata_snapshot unsupported on this kernel"
fi
emit

# ---------------------------------------------------------------------------
# E4: suspend + snapshot latency (the operability number)
# ---------------------------------------------------------------------------
emit "## E4: snapshot coordination"
emit
if dmsetup targets 2>/dev/null | awk '{print $1}' | grep -qx snapshot; then
    snap_name="bc-snap-$$"
    snap_sectors=$((ORIGIN_MB * 1024 * 1024 / 512))
    dd if=/dev/zero of="$work/cow.img" bs=1M count=64 status=none
    cow_loop="$(losetup -f --show "$work/cow.img")"

    start="$(now_ms)"
    if dmsetup create "$snap_name" \
        --table "0 $snap_sectors snapshot $origin_loop $cow_loop P 8" 2>"$work/snap2.err"; then
        end="$(now_ms)"
        emit "- create dm-snapshot: $((end - start)) ms (does not suspend the origin)"
        dmsetup remove "$snap_name" >/dev/null 2>&1 || true
    else
        emit "- dm-snapshot create failed: $(cat "$work/snap2.err")"
    fi
    losetup -d "$cow_loop" >/dev/null 2>&1 || true
else
    emit "- snapshot target unavailable, cannot measure"
fi
emit

emit "## Not measured by this script"
emit
emit "- D2 metadata growth ceiling: requires running past the metadata device size."
emit "- Power-cut behaviour (E5): needs a reboot test or dm-flakey on the metadata device."
emit "- Discard handling (E9): needs a filesystem workload."
emit "- Sustained write throughput with era enabled (E6b)."

if [ -n "$OUT" ]; then
    # Re-run with output captured is the caller's job; this only reports.
    emit
    emit "_Run with \`$0 > $OUT\` to capture._"
fi
