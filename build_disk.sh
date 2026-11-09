#!/bin/bash
# build_disk.sh — Build a directly-bootable HavenDOS VirtIO disk image
#
# Produces havendos_v0.5.9.13_bootable.img:
#   - MBR partition table (one bootable FAT16 partition at sector 2048)
#   - GRUB2 (boot.img in MBR, core.img embedded in the post-MBR gap)
#   - /boot/grub/grub.cfg pointing at /boot/havendos.elf
#   - /boot/havendos.elf (the actual kernel, multiboot1)
#
# Attach the resulting .img as a VirtIO Drive in UTM SE and boot —
# no ISO required. The disk is ALSO a normal FAT16 volume HavenDOS
# itself can mount and write files to once running (fs/fat16.c),
# so files saved from inside the OS persist across reboots and are
# visible here too.
#
# Requirements (all present in this build environment; on your own
# machine install via apt: grub-pc grub-pc-bin mtools fdisk):
#   grub-mkimage, grub-bios-setup, sfdisk, mformat, mcopy, mmd
#
# Usage:
#   ./build_disk.sh [path-to-havendos.elf] [output.img] [size_mb]
#
# Defaults: havendos.elf in the same directory, output havendos_disk.img, 15MB

set -e

ELF="${1:-havendos.elf}"
OUT="${2:-havendos_disk.img}"
SIZE_MB="${3:-15}"

if [ ! -f "$ELF" ]; then
    echo "ERROR: kernel ELF not found at $ELF"
    echo "Usage: $0 [path-to-havendos.elf] [output.img] [size_mb]"
    exit 1
fi

# Resolve to absolute paths BEFORE we cd into the temp workdir
ELF="$(cd "$(dirname "$ELF")" && pwd)/$(basename "$ELF")"
case "$OUT" in
    /*) OUT_ABS="$OUT" ;;
    *)  OUT_ABS="$(pwd)/$OUT" ;;
esac

WORK=$(mktemp -d)
echo "== Building $OUT (${SIZE_MB}MB) from $ELF =="
echo "Workdir: $WORK"

cd "$WORK"

# ── Step 1: blank image ─────────────────────────────────────
echo "[1/7] Creating blank ${SIZE_MB}MB image..."
dd if=/dev/zero of=disk.img bs=1M count="$SIZE_MB" status=none

# ── Step 2: partition table (FAT16, bootable, starts at 2048) ──
echo "[2/7] Writing MBR partition table..."
echo "label: dos
unit: sectors

start=2048, type=6, bootable" | sfdisk disk.img > /dev/null

# ── Step 3: format the partition as FAT16 ───────────────────
echo "[3/7] Formatting partition as FAT16..."
PART_OFFSET=$((2048 * 512))
mformat -i "disk.img@@${PART_OFFSET}" -v HAVENDOS ::

# ── Step 4: create /boot and /boot/grub on the partition ────
echo "[4/7] Creating /boot/grub..."
mmd -i "disk.img@@${PART_OFFSET}" ::/boot
mmd -i "disk.img@@${PART_OFFSET}" ::/boot/grub

# ── Step 5: write grub.cfg and copy the kernel ──────────────
echo "[5/7] Writing grub.cfg and copying kernel..."
cat > grub.cfg << 'EOF'
set timeout=3
set default=0

insmod part_msdos
insmod fat
insmod multiboot

menuentry "HavenDOS v0.5.9.13" {
    set root=(hd0,msdos1)
    multiboot (hd0,msdos1)/boot/havendos.elf
    boot
}
EOF
mcopy -i "disk.img@@${PART_OFFSET}" grub.cfg   ::/boot/grub/grub.cfg
mcopy -i "disk.img@@${PART_OFFSET}" "$ELF"     ::/boot/havendos.elf

# ── Step 6: build GRUB core.img with search (works on any drive number) ──
echo "[6/7] Building and embedding GRUB..."
cat > grub_early.cfg << 'GRUBEOF'
set root=(hd0,msdos1)
set prefix=(hd0,msdos1)/boot/grub
if [ -f ($root)/boot/grub/grub.cfg ]; then
    configfile ($root)/boot/grub/grub.cfg
fi
GRUBEOF

grub-mkimage \
  -O i386-pc \
  -o core.img \
  -p '(hd0,msdos1)/boot/grub' \
  -c grub_early.cfg \
  biosdisk part_msdos fat multiboot normal configfile \
  ls echo reboot halt search search_fs_file

mkdir -p bootdir/i386-pc
cp "$(dirname "$(command -v grub-mkimage)")/../lib/grub/i386-pc/boot.img" bootdir/i386-pc/ 2>/dev/null \
  || cp /usr/lib/grub/i386-pc/boot.img bootdir/i386-pc/
cp core.img bootdir/i386-pc/core.img

echo "(hd0) disk.img" > device.map
grub-bios-setup --directory=bootdir/i386-pc --device-map=device.map disk.img > /dev/null

# ── Step 7: copy result out ──────────────────────────────────
echo "[7/7] Finalising..."
cp disk.img "$OUT_ABS"

cd "$OLDPWD"
rm -rf "$WORK"

SIZE_ACTUAL=$(du -h "$OUT_ABS" | cut -f1)
echo ""
echo "=================================="
echo "  $OUT_ABS built successfully ($SIZE_ACTUAL)"
echo "=================================="
echo ""
echo "Attach as a VirtIO Drive in UTM SE and boot directly —"
echo "no ISO required. GRUB will auto-boot HavenDOS in 3 seconds."
echo ""
echo "Files saved from inside HavenDOS (notepad ^S, write, etc.)"
echo "persist on this same FAT16 partition across reboots."
