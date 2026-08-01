#!/usr/bin/env bash
# Validate the LiveArea assets against what the Vita package installer accepts.
#
# The installer rejects the whole VPK with error 0x8010113D if any sce_sys PNG
# is not 8-bit indexed (PNG colour type 3), or is not exactly the right size.
# That failure happens at install time, not at render time, so it is worth
# checking before packing rather than after flashing to a console.
set -uo pipefail

ROOT="${1:-$(dirname "$0")/..}"
SCE="$ROOT/sce_sys"
fail=0

check() {  # check <file> <w> <h>
  local f=$1 w=$2 h=$3 geom ctype depth
  if [ ! -f "$f" ]; then
    echo "FAIL  $(basename "$f"): missing"; fail=1; return
  fi
  geom=$(identify -format '%wx%h' "$f")
  ctype=$(identify -format '%[png:IHDR.color_type]' "$f" | tr -d ' ')
  depth=$(identify -format '%[png:IHDR.bit_depth]' "$f" | tr -d ' ')

  if [ "$geom" != "${w}x${h}" ]; then
    echo "FAIL  $(basename "$f"): size $geom, need ${w}x${h}"; fail=1; return
  fi
  case "$ctype" in
    3*) ;;
    *)  echo "FAIL  $(basename "$f"): colour type '$ctype', need 3 (indexed)"; fail=1; return ;;
  esac
  if [ "$depth" != "8" ]; then
    echo "FAIL  $(basename "$f"): bit depth $depth, need 8"; fail=1; return
  fi
  echo "ok    $(basename "$f")  $geom  indexed/8-bit  $(stat -c%s "$f") bytes"
}

check "$SCE/icon0.png"                      128 128
check "$SCE/livearea/contents/bg.png"       840 500
check "$SCE/livearea/contents/startup.png"  280 158

[ $fail -eq 0 ] && echo "LiveArea assets OK" || echo "LiveArea assets WILL FAIL TO INSTALL (0x8010113D)"
exit $fail
