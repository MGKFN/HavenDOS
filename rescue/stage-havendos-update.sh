#!/bin/sh
# ====================================================================
#  stage-havendos-update.sh -- put a HavenDOS update on the EFI System
#  Partition from Linux, so `update` can find it.
#
#  The Linux half of rescue/Stage-HavenDOS-Update.ps1. That one is
#  PowerShell and `mountvol`, so it only helps a machine that
#  dual-boots Windows. HavenDOS is also installed beside Linux Mint on
#  a second laptop, and until now there was no way to stage an update
#  on that one at all.
#
#  It writes exactly two files:
#
#      /EFI/HAVENDOS/update/havendos.elf    the new kernel
#      /EFI/HAVENDOS/update/update.inf      version, size and CRC-32
#
#  NOTHING ELSE ON THE ESP IS TOUCHED. It creates one directory and
#  writes those two files. It does not go near /EFI/Microsoft,
#  /EFI/BOOT, /EFI/ubuntu, the firmware boot entries, or anything else
#  already on that partition.
#
#  USAGE -- put this file, havendos.elf and update.inf in one folder:
#
#      sudo ./stage-havendos-update.sh
#
#  or name things explicitly:
#
#      sudo ./stage-havendos-update.sh -k ./havendos.elf \
#                                      -m ./update.inf \
#                                      -d /dev/nvme0n1p1
#
#  Then boot HavenDOS and type:  update
#  Use a boot entry WITHOUT `live` -- a live shell cannot see the
#  HavenDOS partition and `update` refuses with "No FAT16 partition
#  found".
# ====================================================================
set -eu

KERNEL=""
MANIFEST=""
DEVICE=""
DRYRUN=0

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -k|--kernel)   KERNEL=${2:?-k needs a path};   shift 2 ;;
        -m|--manifest) MANIFEST=${2:?-m needs a path}; shift 2 ;;
        -d|--device)   DEVICE=${2:?-d needs a path};   shift 2 ;;
        -n|--dry-run)  DRYRUN=1; shift ;;
        -h|--help)     usage 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; usage 2 ;;
    esac
done

[ -n "$KERNEL" ]   || KERNEL="$HERE/havendos.elf"
[ -n "$MANIFEST" ] || MANIFEST="$HERE/update.inf"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*"; }

# ── the payload ─────────────────────────────────────────────────────
[ -f "$KERNEL" ]   || die "no such file: $KERNEL"
[ -f "$MANIFEST" ] || die "no such file: $MANIFEST"

# ELF magic, so a truncated download or the wrong file is caught before
# anything is mounted.
magic=$(dd if="$KERNEL" bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$magic" = "7f454c46" ] || \
    die "$KERNEL does not start with ELF magic. That is not a HavenDOS kernel."

size=$(wc -c < "$KERNEL" | tr -d ' ')

command -v python3 >/dev/null 2>&1 || \
    die "python3 is needed to check the CRC-32 (it is zlib's). Install it, or stage by hand."
crc=$(python3 - "$KERNEL" <<'PY'
import sys, zlib
with open(sys.argv[1], 'rb') as f:
    print('%08x' % (zlib.crc32(f.read()) & 0xFFFFFFFF))
PY
)

say "kernel   : $KERNEL"
say "size     : $size bytes"
say "crc32    : $crc"

# ── the manifest must describe THIS kernel ──────────────────────────
# Same check Stage-HavenDOS-Update.ps1 performs, and for the same
# reason: `update` verifies the checksum before it writes anything, so
# a mismatched pair would be staged successfully and then refused on
# the machine, which is a confusing place to find out.
m_size=$(sed -n 's/^[[:space:]]*size[[:space:]]*=[[:space:]]*\([0-9]\{1,\}\).*/\1/p' "$MANIFEST" | head -1)
m_crc=$(sed  -n 's/^[[:space:]]*crc32[[:space:]]*=[[:space:]]*\(0[xX]\)\{0,1\}\([0-9a-fA-F]\{1,\}\).*/\2/p' "$MANIFEST" | head -1)
m_ver=$(sed  -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*\([^[:space:]]\{1,\}\).*/\1/p' "$MANIFEST" | head -1)

[ -n "$m_size" ] || die "$MANIFEST has no size= line. Is it really update.inf?"
[ "$m_size" = "$size" ] || \
    die "the manifest describes a ${m_size}-byte kernel and this one is $size bytes. Refusing."

if [ -n "$m_crc" ]; then
    # Lower-case both sides; the file may carry either.
    lhs=$(printf '%s' "$m_crc" | tr 'A-F' 'a-f')
    [ "$lhs" = "$crc" ] || \
        die "the manifest's crc32 ($lhs) does not match this kernel ($crc). Refusing."
    say "manifest : size and crc32 both match this kernel (v${m_ver:-?})"
else
    say "manifest : size matches (it carries no crc32)"
fi

# ── find the EFI System Partition ───────────────────────────────────
ESP_GUID=c12a7328-f81f-11d2-ba4b-00a0c93ec93b

find_esp() {
    command -v lsblk >/dev/null 2>&1 || return 1
    # PARTTYPE is the GPT partition type GUID. Matching on that rather
    # than on a mountpoint or a label, because an ESP is identified by
    # its type and nothing else is.
    lsblk -rno PATH,PARTTYPE 2>/dev/null | while read -r path ptype; do
        [ "$(printf '%s' "$ptype" | tr 'A-Z' 'a-z')" = "$ESP_GUID" ] && \
            printf '%s\n' "$path"
    done
}

if [ -z "$DEVICE" ]; then
    found=$(find_esp || true)
    n=$(printf '%s\n' "$found" | grep -c . || true)
    if [ "$n" -eq 0 ]; then
        die "no EFI System Partition found. If this machine booted in legacy BIOS mode it has none; otherwise pass one with -d."
    elif [ "$n" -gt 1 ]; then
        say ""
        say "more than one EFI System Partition:"
        printf '%s\n' "$found" | sed 's/^/  /'
        die "pass the one you want with -d"
    fi
    DEVICE=$(printf '%s\n' "$found")
fi

say "esp      : $DEVICE"

[ -b "$DEVICE" ] || die "$DEVICE is not a block device"

if [ "$DRYRUN" -eq 1 ]; then
    say ""
    say "dry run: everything checks out, nothing was mounted or written."
    exit 0
fi

[ "$(id -u)" = "0" ] || die "run this with sudo -- mounting the ESP needs root."

# ── mount it, or use the mount it already has ───────────────────────
MOUNTED_BY_US=0
MNT=$(findmnt -nro TARGET --source "$DEVICE" 2>/dev/null | head -1 || true)

cleanup() {
    if [ "$MOUNTED_BY_US" = "1" ] && [ -n "${MNT:-}" ]; then
        umount "$MNT" 2>/dev/null || true
        rmdir "$MNT" 2>/dev/null || true
    fi
}
trap cleanup EXIT HUP INT TERM

if [ -n "$MNT" ]; then
    say "already mounted at $MNT -- using it, and leaving it mounted"
else
    MNT=$(mktemp -d /tmp/havendos-esp.XXXXXX)
    mount -t vfat -o rw "$DEVICE" "$MNT" || die "could not mount $DEVICE"
    MOUNTED_BY_US=1
    say "mounted at $MNT"
fi

# A sanity check that we are on an ESP and not somebody's data
# partition: an ESP that the firmware boots from has an /EFI directory.
[ -d "$MNT/EFI" ] || [ -d "$MNT/efi" ] || \
    die "$DEVICE has no /EFI directory -- that does not look like an EFI System Partition. Refusing to write."

DEST="$MNT/EFI/HAVENDOS/update"
mkdir -p "$DEST"

cp -f "$KERNEL"   "$DEST/havendos.elf"
cp -f "$MANIFEST" "$DEST/update.inf"
sync

# ── read it back ────────────────────────────────────────────────────
# FAT on removable-ish media and a copy that reported success are not
# the same thing as bytes on the disk.
back_size=$(wc -c < "$DEST/havendos.elf" | tr -d ' ')
back_crc=$(python3 - "$DEST/havendos.elf" <<'PY'
import sys, zlib
with open(sys.argv[1], 'rb') as f:
    print('%08x' % (zlib.crc32(f.read()) & 0xFFFFFFFF))
PY
)
[ "$back_size" = "$size" ] || die "readback is $back_size bytes, expected $size"
[ "$back_crc" = "$crc" ]   || die "readback crc32 is $back_crc, expected $crc"

say ""
say "wrote, and read back byte-identical:"
say "  /EFI/HAVENDOS/update/havendos.elf   $size bytes, crc32=$crc"
say "  /EFI/HAVENDOS/update/update.inf     v${m_ver:-?}"
say ""
say "Nothing else on the ESP was touched."
say ""
say "Now boot HavenDOS and type:  update"
say "Use a boot entry WITHOUT 'live' -- a live shell cannot see the"
say "HavenDOS partition and update will refuse with"
say '"No FAT16 partition found".'
