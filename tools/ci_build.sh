#!/bin/sh
# ci_build.sh -- build eboot.bin inside the vitasdk/vitasdk image (see
# .github/workflows/build-vpk.yml). Run from the repository root.
#
# vitaGL is linked statically and must carry this repository's patch, built
# with the flags SETUP.md section 4 requires, so the SDK's prebuilt copy is
# replaced before the loader is configured.
set -eu

VITAGL_COMMIT=38d2f9704b6b241965ed7086aeaacd69f044f2c5
ROOT="$(pwd)"

apk add --no-cache git cmake make python3 bash >/dev/null 2>&1 || true

rm -rf /tmp/vitaGL
git clone -q https://github.com/Rinnegatamante/vitaGL /tmp/vitaGL
cd /tmp/vitaGL
git checkout -q "$VITAGL_COMMIT"
if git apply --unidiff-zero "$ROOT/patches/vitaGL-packed-vbo-offset.patch"; then
  echo "vitaGL: applied vitaGL-packed-vbo-offset.patch"
else
  git apply "$ROOT/patches/vitagl-vbo-offset-64k.patch"
  echo "vitaGL: applied vitagl-vbo-offset-64k.patch"
fi
make clean >/dev/null
make LOG_ERRORS=1 HAVE_SHADER_CACHE=1 -j"$(nproc)"
make install

cd "$ROOT"
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
arm-vita-eabi-gcc --version | head -1
cmake --build build --target KOTOR -j"$(nproc)"

# vita-elf-create needs ~4.6 KB free between the RX segment's end and the RW
# segment's start, and how much there is depends on the size of .text. If the
# gap is short, pad .rodata just past the 64 KB boundary (loader/sce_pad.c).
NEED=8192
gap() {
  arm-vita-eabi-readelf -lW build/KOTOR | awk '$1 == "LOAD" { print $3, $6 }' | {
    read rx_va rx_mem; read rw_va rw_mem
    echo $(( rw_va - (rx_va + rx_mem) ))
  }
}
G=$(gap)
echo "RX->RW gap: $G bytes (need $NEED)"
if [ "$G" -lt "$NEED" ]; then
  PAD=$(( (G + 256 + 3) / 4 * 4 ))
  echo "padding .rodata by $PAD bytes"
  cmake -S . -B build -DKOTOR_SCE_PAD="$PAD"
  cmake --build build --target KOTOR -j"$(nproc)"
  echo "RX->RW gap now: $(gap) bytes"
fi
cmake --build build -j"$(nproc)"
ls -l build/eboot.bin build/KOTOR.vpk
