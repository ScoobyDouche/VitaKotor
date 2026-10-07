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
arm-vita-eabi-readelf -lW build/KOTOR | sed -n '1,30p'
cmake --build build -j"$(nproc)"
ls -l build/eboot.bin build/KOTOR.vpk
