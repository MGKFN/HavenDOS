#!/bin/sh
# Assemble the 0.8 Beta release: ISO, source archive, changelog, handoff,
# the manual and the screenshots that illustrate it.
set -e
SRC=/home/claude/havendos/HavenDOS-0.7.20-source
PKG=/home/claude/havendos/pkg
SHOTS=/home/claude/havendos/screenshots
MAN=/home/claude/havendos/manual

rm -rf "$PKG"
mkdir -p "$PKG"

cp "$SRC/HavenDOS-0.8.0.iso"            "$PKG/"
cp "$SRC/CHANGELOG-v0.8.0-Beta.md"      "$PKG/"
cp "$SRC/HANDOFF.md"                    "$PKG/"
cp "$MAN/HavenDOS-0.8-Beta-Manual.pdf"  "$PKG/"
mkdir -p "$PKG/screenshots"
cp "$SHOTS"/*.png "$SHOTS/index.txt"    "$PKG/screenshots/"

# The source archive: everything that builds, nothing that is generated.
# include/kernel_elf.h is 3.9 MB of hex the two-pass build regenerates.
STAGE=$(mktemp -d)
mkdir -p "$STAGE/HavenDOS-0.8.0-source"
( cd "$SRC" && tar cf - \
    --exclude='*.o' --exclude='*.d' --exclude='*.elf' --exclude='*.iso' \
    --exclude='include/kernel_elf.h' --exclude='isodir/boot/havendos.elf' \
    . ) | ( cd "$STAGE/HavenDOS-0.8.0-source" && tar xf - )
cp -r "$MAN"                            "$STAGE/HavenDOS-0.8.0-source/manual"
rm -f "$STAGE/HavenDOS-0.8.0-source/manual/manual.html"
mkdir -p "$STAGE/HavenDOS-0.8.0-source/harness"
cp /home/claude/havendos/harness/*.py   "$STAGE/HavenDOS-0.8.0-source/harness/"
cp /home/claude/havendos/harness/*.sh   "$STAGE/HavenDOS-0.8.0-source/harness/" 2>/dev/null || true
( cd "$STAGE" && zip -qr "$PKG/HavenDOS-0.8.0-source.zip" HavenDOS-0.8.0-source )
rm -rf "$STAGE"

echo
ls -la "$PKG"
echo
echo "screenshots: $(ls "$PKG/screenshots"/*.png | wc -l)"
