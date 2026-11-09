#!/bin/sh
# make_release.sh -- the source archive and the ISO, named from version.h
#
# The archive contains everything that builds and nothing that is
# generated, and the list of what counts as generated is NOT written here.
# It is `make clean`, run inside the copy, because the Makefile is where
# that list already lives and a second copy of it drifts.
#
# It drifted once already: a hand-written release script deleted
# tests/*.inc on the grounds that .inc files are generated -- true of
# bars.inc, vercmp.inc, appreg.inc, fat32_fixture.inc and esp_fixture.inc,
# and false of tests/gptdisk.inc, which is source. The archive built, and
# `make test` inside it stopped at the first suite that included it. The
# archive's own test run is what noticed, which is why this script ends by
# doing exactly that.
set -e

SRC=$(cd "$(dirname "$0")/.." && pwd)
VER=$(sed -n 's/^#define[[:space:]]*HAVENDOS_VER_NUM[[:space:]]*"\([^"]*\)".*/\1/p' \
      "$SRC/include/version.h")
NAME=$(sed -n 's/^#define[[:space:]]*HAVENDOS_CODENAME[[:space:]]*"\([^"]*\)".*/\1/p' \
      "$SRC/include/version.h")
OUT=${1:-/tmp/havendos-release}
DIR="HavenDOS-$VER-source"

echo "HavenDOS $VER \"$NAME\""
rm -rf "$OUT"
mkdir -p "$OUT"
cp -r "$SRC" "$OUT/$DIR"
cd "$OUT/$DIR"

make clean > /dev/null 2>&1 || true
find . -name '*.o' -delete
find . -name '*.d' -delete
find . -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
rm -f havendos.iso HavenDOS-*.iso havendos.elf havendos_pass1.elf

cd "$OUT"
tar czf "$OUT/HavenDOS-$VER-source.tar.gz" "$DIR"
echo "  $OUT/HavenDOS-$VER-source.tar.gz  $(du -h "$OUT/HavenDOS-$VER-source.tar.gz" | cut -f1)"

# Build it from the archive alone, in a directory of its own, and run the
# suite there. An archive that does not do both is not a release.
VFY="$OUT/verify"
rm -rf "$VFY"; mkdir -p "$VFY"
tar xzf "$OUT/HavenDOS-$VER-source.tar.gz" -C "$VFY"
cd "$VFY/$DIR"
echo "  building from the archive alone..."
make > "$OUT/build.log" 2>&1 || { echo "  BUILD FAILED -- see $OUT/build.log"; exit 1; }
echo "  running the suite from the archive alone..."
make test > "$OUT/test.log" 2>&1 || { echo "  TESTS FAILED -- see $OUT/test.log"; exit 1; }
grep -oE '^[0-9]+/[0-9]+ passed' "$OUT/test.log" | awk -F'[/ ]' \
    '{p+=$1; t+=$2; n++} END {printf "  %d suites, %d/%d checks\n", n, p, t}'
cp "HavenDOS-$VER.iso" "$OUT/"
echo "  $OUT/HavenDOS-$VER.iso  $(du -h "$OUT/HavenDOS-$VER.iso" | cut -f1)"
echo "ok"
