#!/usr/bin/env bash
# BlockChronicle capability probe for the dm-era backend.
#
# Run on the target Linux host, as root. Exits 0 only if every required
# capability is present. Nothing here is assumed from the distribution name:
# every check interrogates the kernel.
#
# This script is deliberately not part of CI: it needs root, device mapper and
# loop devices.

set -uo pipefail

PASS=0
FAIL=0
SKIP=0

report() { printf '%-6s %s\n' "$1" "$2"; }
ok()     { report PASS "$1"; PASS=$((PASS + 1)); }
bad()    { report FAIL "$1"; FAIL=$((FAIL + 1)); }
skip()   { report SKIP "$1"; SKIP=$((SKIP + 1)); }
info()   { report INFO "$1"; }

need_root() { [ "$(id -u)" -eq 0 ]; }

# 1. Module present or loadable.
if [ -d /sys/module/dm_era ]; then
    ok "dm_era module is loaded"
elif modprobe dm_era 2>/dev/null; then
    ok "dm_era module loaded by modprobe"
else
    bad "dm_era module unavailable (modprobe failed)"
fi

# 2. Device mapper advertises the era target.
if command -v dmsetup >/dev/null 2>&1; then
    if dmsetup targets 2>/dev/null | awk '{print $1}' | grep -qx era; then
        ok "dmsetup reports the 'era' target"
    else
        bad "dmsetup has no 'era' target"
    fi
else
    bad "dmsetup is not installed"
fi

info "kernel $(uname -r) on $(uname -m)"

# 3. Userspace metadata tools.
for tool in era_dump era_check era_restore; do
    if command -v "$tool" >/dev/null 2>&1; then
        ok "$tool present"
    else
        bad "$tool missing (install thin-provisioning-tools)"
    fi
done

# 4. Snapshot providers.
if dmsetup targets 2>/dev/null | awk '{print $1}' | grep -qx snapshot; then
    ok "'snapshot' target present"
else
    bad "'snapshot' target absent"
fi

if dmsetup targets 2>/dev/null | awk '{print $1}' | grep -qx thin-pool; then
    ok "'thin-pool' target present"
else
    skip "'thin-pool' absent (dm-thin snapshots unavailable)"
fi

# 5. Control node reachable.
if [ -w /dev/mapper/control ]; then
    ok "/dev/mapper/control is writable"
else
    bad "/dev/mapper/control is not writable (need root and CAP_SYS_ADMIN)"
fi

# 6. End-to-end scratch cycle. This is the only check that proves the whole path.
if ! need_root; then
    skip "scratch era cycle (requires root)"
else
    work="$(mktemp -d)"
    meta_loop=""
    origin_loop=""
    dm_name="bc-probe-$$"

    cleanup() {
        dmsetup remove "$dm_name" >/dev/null 2>&1 || true
        [ -n "$meta_loop" ] && losetup -d "$meta_loop" >/dev/null 2>&1 || true
        [ -n "$origin_loop" ] && losetup -d "$origin_loop" >/dev/null 2>&1 || true
        rm -rf "$work"
    }
    trap cleanup EXIT

    dd if=/dev/zero of="$work/meta.img" bs=1M count=16 status=none
    dd if=/dev/zero of="$work/origin.img" bs=1M count=64 status=none

    meta_loop="$(losetup -f --show "$work/meta.img" 2>/dev/null)"
    origin_loop="$(losetup -f --show "$work/origin.img" 2>/dev/null)"

    if [ -z "$meta_loop" ] || [ -z "$origin_loop" ]; then
        bad "scratch era cycle: could not allocate loop devices"
    else
        sectors=$((64 * 1024 * 1024 / 512))
        blocks=$((sectors / 128)) # 128 sectors == 64 KiB

        if dmsetup create "$dm_name" \
            --table "0 $sectors era $meta_loop $origin_loop 128 $blocks 1" 2>"$work/err"; then
            if dmsetup message "$dm_name" 0 checkpoint 2>>"$work/err"; then
                ok "scratch era cycle: create, checkpoint"
                dmsetup status "$dm_name" 2>&1 | sed 's/^/       status: /'
            else
                bad "scratch era cycle: checkpoint failed: $(cat "$work/err")"
            fi
        else
            bad "scratch era cycle: create failed: $(cat "$work/err")"
        fi
    fi
fi

echo
echo "pass=$PASS fail=$FAIL skip=$SKIP"
if [ "$FAIL" -ne 0 ]; then
    echo "result: dm-era backend is NOT usable on this host"
    exit 1
fi
echo "result: required capabilities present (run experiments.sh for measurements)"
exit 0
