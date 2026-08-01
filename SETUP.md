# PS Vita Homebrew Dev Environment — Setup

This document records every step used to set up a working PS Vita homebrew
toolchain on this machine, build **vitaGL** from source, and confirm the whole
pipeline by compiling sample `.vpk` packages.

- **Date:** 2026-07-18
- **Host:** Linux Mint 22.3 (Ubuntu 24.04 base), x86_64, kernel 6.17
- **Result:** ✅ Fully working — `arm-vita-eabi-gcc 15.2.0`, vitaGL built from
  source, and two sample VPKs produced.

---

## 0. Layout & key decision (no root)

| Path | Purpose |
|------|---------|
| `~/vitasdk` | The SDK install (`$VITASDK`). **924 MB** after full install. |
| `~/vitadev/vdpm` | The vdpm package manager (cloned). |
| `~/vitadev/vitaGL` | vitaGL source (cloned + built). |
| `~/vitadev/samples` | Official VitaSDK samples. |

> **Why `~/vitasdk` instead of the default `/usr/local/vitasdk`?**
> The official instructions install to `/usr/local/vitasdk`, which is **not
> writable** by this user, and `sudo` requires a password that isn't available
> non-interactively here. VitaSDK's tooling honors the `$VITASDK` environment
> variable, so installing into a user-writable home directory is fully
> supported and needs no root. See the workaround in **step 2**.

---

## 1. Prerequisites

All required build tools were already present (verified with `command -v`):

```
git  cmake (3.28.3)  make  gcc/g++ (13.3.0, host)  wget  curl  tar  bzip2  xz  python3  pkg-config
```

The official prerequisite package set, for reference on a fresh machine:

```bash
sudo apt-get install make git-core cmake python3
```

---

## 2. Install the VitaSDK toolchain via vdpm

Set the environment variables and persist them to `~/.bashrc`:

```bash
# Appended to ~/.bashrc
export VITASDK=$HOME/vitasdk
export PATH=$VITASDK/bin:$PATH
```

Clone vdpm:

```bash
mkdir -p ~/vitadev && cd ~/vitadev
git clone https://github.com/vitasdk/vdpm      # HEAD e423be4
```

**Standard flow (needs write access to `$VITASDK`, uses sudo for
`/usr/local`):**

```bash
cd ~/vitadev/vdpm
./bootstrap-vitasdk.sh      # downloads + extracts the prebuilt toolchain
./install-all.sh           # installs all prebuilt libraries
```

**Workaround actually used here (no root).** `bootstrap-vitasdk.sh` refuses to
run if `$VITASDK` already exists, while its internal `install_vitasdk` only
falls back to `sudo mkdir`/`chown` when the directory is *missing*. So we
**pre-create** the writable target and call the installer function directly —
this runs the exact same download+extract payload, minus the root requirement:

```bash
export VITASDK=$HOME/vitasdk
export PATH=$VITASDK/bin:$PATH
mkdir -p "$VITASDK"                        # pre-create (writable) → skips the sudo branch
cd ~/vitadev/vdpm
source include/install-vitasdk.sh
install_vitasdk "$VITASDK"                 # wget <toolchain>.tar.bz2 | tar xj -C $VITASDK
```

This downloaded the autobuild toolchain
`vitasdk-x86_64-linux-gnu-2026-05-24_12-05-02.tar.bz2` (~110 MB) and extracted
it to `$VITASDK`. Installed toolchain build stamp:

```
Built at 2026-05-24 12:26:53
vita-headers   e37621b
vita-toolchain 71f3789
newlib         fbb8375
```

### Install the prebuilt libraries

```bash
cd ~/vitadev/vdpm
./install-all.sh
```

This installed **105 packages** (zlib, libpng, freetype, libvita2d, SDL/SDL2/SDL3,
taihen, kubridge, a prebuilt vitaGL, etc.). Exit code 0, no errors.

---

## 3. Verify `arm-vita-eabi-gcc`

```bash
$ arm-vita-eabi-gcc --version
arm-vita-eabi-gcc (GNU Tools for ARM Embedded Processors) 15.2.0
```

Standalone compile + link sanity check:

```bash
cat > /tmp/hello.c <<'EOF'
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
int main(){ printf("hello vita\n"); sceKernelExitProcess(0); return 0; }
EOF
arm-vita-eabi-gcc -Wl,-q /tmp/hello.c -o /tmp/hello.elf
file /tmp/hello.elf
# → ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV) ✅
```

The packaging tools (`vita-elf-create`, `vita-make-fself`, `vita-mksfoex`,
`vita-pack-vpk`) are all present in `$VITASDK/bin`.

---

## 4. Build vitaGL from source

The `install-all.sh` step already dropped a *prebuilt* `libvitaGL.a` into the
SDK. Per the task, we clone and build it **from source**, overwriting that copy:

```bash
cd ~/vitadev
git clone https://github.com/Rinnegatamante/vitaGL     # HEAD 38d2f97
cd vitaGL
make clean
make LOG_ERRORS=1 -j$(nproc)   # produces libvitaGL.a
make install                   # copies libvitaGL.a → $VITASDK/arm-vita-eabi/lib/
                               #        source/vitaGL.h → $VITASDK/arm-vita-eabi/include/
```

> **`LOG_ERRORS=1` is required.** A flagless `make` fails to link the loader with
> `undefined reference to shark_log_cb` — `gxm.c` calls it, but
> `custom_shaders.c` only defines it under `HAVE_SHARK_LOG` *or* `LOG_ERRORS`.
> This flag is what the working build has always used; the plain `make` recorded
> here originally was accurate only for the first session and then drifted.
> Always `make clean` when changing flags, or you get a half-and-half archive
> whose objects disagree about the feature macros.

Build completed with exit 0 (only benign warnings). Verified the installed
`$VITASDK/arm-vita-eabi/lib/libvitaGL.a` matches the freshly-built artifact.

---

## 5. Confirm end-to-end by compiling sample VPKs

### 5a. Official CMake sample — `hello_world`

```bash
cd ~/vitadev
git clone https://github.com/vitasdk/samples          # HEAD 99194cf
cd samples/hello_world
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" ..
make -j$(nproc)
```

Produced **`hello_world.vpk`** (46 KB). Contents confirm a valid Vita package:

```
sce_sys/param.sfo
eboot.bin
sce_sys/icon0.png
sce_sys/livearea/contents/{bg.png,startup.png,template.xml}
```

### 5b. vitaGL sample — `rotating_cube` (proves the from-source lib links)

Built the Makefile-based vitaGL sample, which links `-lvitaGL`:

```bash
cd ~/vitadev/vitaGL/samples/rotating_cube
make -j$(nproc) CFLAGS="-g -Wl,-q -O2 -ftree-vectorize -include string.h -Wno-implicit-function-declaration"
```

Produced **`rotating_cube.vpk`** (2.8 MB), linked against the from-source
`libvitaGL.a`. ✅

> **Note on the extra CFLAGS:** the stock sample `main.c` uses `memcpy` without
> `#include <string.h>`. GCC 15 defaults to C23, where an implicit function
> declaration is a hard **error** (not a warning as in older GCC). This is a
> source bug in the *sample*, not a toolchain problem, so we inject the header
> with `-include string.h` (and relax the diagnostic) to build it unmodified.
> The `hello_world.vpk` in 5a builds with zero extra flags.

---

## Summary of installed versions

| Component | Version / commit |
|-----------|------------------|
| VitaSDK toolchain | autobuild `2026-05-24` |
| `arm-vita-eabi-gcc` | 15.2.0 |
| CMake (host) | 3.28.3 |
| vdpm | `e423be4` |
| vitaGL | `38d2f97` (built from source) |
| samples | `99194cf` |

## Using the environment in a new shell

`~/.bashrc` already exports the variables, so a new terminal is ready to go.
For a non-login/non-interactive shell:

```bash
export VITASDK=$HOME/vitasdk
export PATH=$VITASDK/bin:$PATH
```

Quick smoke test:

```bash
arm-vita-eabi-gcc --version
(cd ~/vitadev/samples/hello_world && rm -rf build && mkdir build && cd build \
  && cmake -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake .. && make)
```

## Maintenance

- **Update the SDK/packages:** `~/vitadev/vdpm/vitasdk-update`
- **(Re)install a single package:** `~/vitadev/vdpm/vdpm -f <name>` (e.g. `vitaGL`)
- **Rebuild vitaGL after a source pull:** `cd ~/vitadev/vitaGL && git pull && make -j$(nproc) && make install`
- **Remove everything:** `rm -rf ~/vitasdk` (and the `VITASDK`/`PATH` lines in `~/.bashrc`)
