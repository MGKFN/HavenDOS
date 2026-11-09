#!/bin/bash
# Assemble the 0.8.1 handoff package: ISO, source archive, handoff, plan.
set -e
SRC=/home/claude/build081
PKG=/home/claude/rel811

rm -rf "$PKG"; mkdir -p "$PKG"
cp "$SRC/HavenDOS-0.8.1.iso" "$PKG/"

# Source archive: everything that builds, nothing that is generated.
# include/kernel_elf.h is megabytes of hex the two-pass build regenerates.
STAGE=$(mktemp -d)
mkdir -p "$STAGE/HavenDOS-0.8.1-source"
( cd "$SRC" && tar cf - \
    --exclude='*.o' --exclude='*.d' --exclude='*.elf' --exclude='*.iso' \
    --exclude='include/kernel_elf.h' --exclude='include/grub_disk_embed.h' \
    --exclude='isodir/boot/havendos.elf' --exclude='tests/appreg.inc' \
    . ) | ( cd "$STAGE/HavenDOS-0.8.1-source" && tar xf - )
( cd "$STAGE" && zip -qr "$PKG/HavenDOS-0.8.1-source.zip" HavenDOS-0.8.1-source )
rm -rf "$STAGE"
echo; ls -la "$PKG"
