#!/usr/bin/env python3
"""Score a KOTOR APK against this loader, without a Vita.

    python3 tools/check_apk.py <path-to.apk>

Reads the native libraries straight out of the APK (an APK is a zip) and
compares what they ask for against what loader/ provides. Prints either
"looks supported" or the list of what is missing -- which is the whole cost
of supporting that build.

Python 3.6+, standard library only. No binutils, no toolchain, no unzipping.
"""

import os, re, struct, sys, zipfile

# ---------------------------------------------------------------- ELF reading

SHT_DYNSYM = 11
STT_OBJECT, STT_FUNC = 1, 2
STB_WEAK = 2


class Elf:
    """Just enough ELF32 little-endian to read .dynsym and a few code bytes."""

    def __init__(self, blob):
        if blob[:4] != b"\x7fELF" or blob[4] != 1 or blob[5] != 1:
            raise ValueError("not a 32-bit little-endian ELF")
        self.b = blob
        phoff, = struct.unpack_from("<I", blob, 0x1C)
        shoff, = struct.unpack_from("<I", blob, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", blob, 0x2A)
        shentsize, shnum = struct.unpack_from("<HH", blob, 0x2E)

        self.loads = []
        for i in range(phnum):
            t, off, va, _, fsz, _, _, _ = struct.unpack_from(
                "<8I", blob, phoff + i * phentsize)
            if t == 1:
                self.loads.append((va, off, fsz))

        self.symbols = []   # (name, value, size, type, undefined, weak)
        for i in range(shnum):
            name, typ, _, _, off, size, link, _, _, ent = struct.unpack_from(
                "<10I", blob, shoff + i * shentsize)
            if typ != SHT_DYNSYM:
                continue
            _, _, _, _, stroff, strsize, _, _, _, _ = struct.unpack_from(
                "<10I", blob, shoff + link * shentsize)
            strtab = blob[stroff:stroff + strsize]
            for j in range(size // 16):
                nm, val, sz, info, _, shndx = struct.unpack_from(
                    "<IIIBBH", blob, off + j * 16)
                end = strtab.find(b"\0", nm)
                s = strtab[nm:end].decode("utf-8", "replace").split("@")[0]
                if s:
                    self.symbols.append((s, val, sz, info & 0xF,
                                         shndx == 0, (info >> 4) == STB_WEAK))

    def defined(self, kind=None):
        return {s: (v, sz) for s, v, sz, t, und, _ in self.symbols
                if not und and (kind is None or t == kind)}

    def imports(self):
        return {s for s, _, _, _, und, weak in self.symbols if und and not weak}

    def code(self, vaddr, n):
        a = vaddr & ~1
        for va, off, fsz in self.loads:
            if va <= a < va + fsz:
                return self.b[off + (a - va):off + (a - va) + n]
        return b""


# ------------------------------------------------- what the loader provides

def loader_symbols(root):
    """Scan loader/*.c for what the loader provides, binds, and trampolines.

    Only the hook_named* hooks keep the original function reachable, so only
    those care what the prologue looks like. A hook_addr/hook_thumb that
    replaces a function outright can overwrite anything it likes.
    """
    provided, referenced, trampolined = set(), set(), set()
    d = os.path.join(root, "loader")
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".c"):
            continue
        src = open(os.path.join(d, fn), encoding="utf-8", errors="replace").read()
        provided |= set(re.findall(r'\{\s*"([^"]+)"\s*,', src))
        referenced |= set(re.findall(r'"(_Z[A-Za-z0-9_]+|g_[A-Za-z0-9_]+)"', src))
        trampolined |= set(re.findall(r'hook_named(?:_port)?\(\s*"([^"]+)"', src))
    return provided, referenced - provided, trampolined


# ------------------------------------------------------- the prologue check

def pc_relative(code):
    """Best-effort: does this Thumb prologue load from the literal pool?

    A hook overwrites the first 8 bytes, so a PC-relative load in there breaks
    the trampoline silently. Covers the common encodings, not every one.
    """
    hits, i = [], 0
    while i + 1 < len(code):
        hw = code[i] | (code[i + 1] << 8)
        if 0x4800 <= hw <= 0x4FFF:
            hits.append("ldr rN,[pc,#..]")
        elif 0xA000 <= hw <= 0xA7FF:
            hits.append("adr rN,..")
        elif hw in (0xF85F, 0xF8DF):
            hits.append("ldr.w rN,[pc,#..]")
        i += 4 if 0xE800 <= hw <= 0xFFFF else 2   # 32-bit Thumb encodings
    return hits


# ------------------------------------------------------------------- report

CORE = ("libKOTOR.so", "libandroid_port.so")
EXTRA = ("libminiz.so", "libLzmaLib.so")


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip())
        return 2
    apk = argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    provided, referenced, trampolined = loader_symbols(root)

    try:
        z = zipfile.ZipFile(apk)
    except Exception as e:
        print("cannot open %s: %s" % (apk, e))
        return 2

    libs, names = {}, {}
    for entry in z.namelist():
        if "/lib/" in "/" + entry and entry.endswith(".so"):
            base = entry.rsplit("/", 1)[-1]
            if "armeabi" in entry:
                names[base] = entry
    if not names:
        print("no armeabi native libraries in %s" % os.path.basename(apk))
        return 2

    print("APK: %s" % os.path.basename(apk))
    print("libraries: %s\n" % ", ".join(sorted(names)))

    if "libfmodex.so" in names or "libgnustl_shared.so" in names:
        print("!! This is a pre-FMOD build (libfmodex / libgnustl_shared).")
        print("   Not supported -- it needs a second audio backend and an")
        print("   older C++ runtime. Details below.\n")

    for base, entry in names.items():
        if base in CORE + EXTRA:
            try:
                libs[base] = Elf(z.read(entry))
            except ValueError as e:
                print("%s: %s" % (base, e))
                return 2

    missing_libs = [l for l in CORE if l not in libs]
    if missing_libs:
        print("!! missing required libraries: %s" % ", ".join(missing_libs))
        return 1

    defined = set()
    for lib in libs.values():
        defined |= set(lib.defined())

    wanted = set()
    for l in CORE:
        wanted |= libs[l].imports()
    unresolved = sorted(wanted - provided - defined)

    print("unresolved imports: %d" % len(unresolved))
    for u in unresolved:
        print("    %s" % u)
    if unresolved:
        print()

    absent = sorted(s for s in referenced
                    if not any(s in lib.defined() for lib in libs.values()))
    hooked = len(referenced) - len(absent)
    print("symbols the loader binds to: %d found, %d missing" % (hooked, len(absent)))
    for a in absent:
        print("    %s" % a)

    risky = []
    for s in sorted(trampolined):
        for lib in libs.values():
            f = lib.defined(STT_FUNC).get(s)
            if f:
                hits = pc_relative(lib.code(f[0], 8))
                if hits:
                    risky.append((s, hits))
                break
    print("trampolined prologues that would break (%d checked): %d"
          % (len(trampolined), len(risky)))
    for s, h in risky:
        print("    %s  (%s)" % (s, ", ".join(h)))

    ok = not unresolved and not absent and not risky
    print("\n%s" % ("LOOKS SUPPORTED -- nothing missing. Still needs a real boot "
                    "to confirm."
                    if ok else
                    "NOT SUPPORTED AS-IS -- the lists above are the whole gap."))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
