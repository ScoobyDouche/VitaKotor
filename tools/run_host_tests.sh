#!/bin/sh
# Build and run the host-side unit tests (tools/test_*.c) with the system gcc.
# Tests that need game data (main.obb, .ncs files) are skipped when it is absent.
# Usage: tools/run_host_tests.sh   (from the repo root or anywhere)
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/vitakotor-host-tests"
mkdir -p "$OUT"
CC="${CC:-gcc}"
STRICT="-std=c99 -Wall -Wextra -Werror -I loader"
fail=0

run() {   # name, then the command
  name=$1; shift
  printf '%-22s ' "$name"
  if out=$("$@" 2>&1); then echo "ok"; else echo "FAILED"; echo "$out" | tail -20; fail=1; fi
}

build() { $CC "$@"; }

build $STRICT -o "$OUT/test_audio_ring" tools/test_audio_ring.c loader/audio_ring.c
run audio_ring "$OUT/test_audio_ring"

build $STRICT -o "$OUT/test_ini" tools/test_ini.c loader/ini.c
run ini "$OUT/test_ini"

build $STRICT -o "$OUT/test_trname" tools/test_trname.c loader/trname.c
run trname "$OUT/test_trname"

build -std=c99 -Wall -Wextra -I tools/hoststubs -I loader \
  -o "$OUT/test_bink_audio_queue" tools/test_bink_audio_queue.c loader/bink_audio_queue.c
run bink_audio_queue "$OUT/test_bink_audio_queue"

if [ -e com.aspyr.swkotor/main.obb ]; then
  build -std=c99 -Wall -Wextra -I tools/hoststubs -I loader \
    -o "$OUT/test_audio_codec" tools/test_audio_codec.c loader/audio_mp3.c
  run audio_codec "$OUT/test_audio_codec" com.aspyr.swkotor/main.obb
else
  printf '%-22s skipped (no com.aspyr.swkotor/main.obb)\n' audio_codec
fi

# test_script_fix needs the trysteal/sandsteal .ncs scripts extracted from the
# game; pass them as arguments by hand (see the top of tools/test_script_fix.c).

exit $fail
