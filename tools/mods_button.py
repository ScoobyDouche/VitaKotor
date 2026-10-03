#!/usr/bin/env python3
"""Build the main menu's "MODS" button from the game's own button art.

The Google Play button opens the mod menu on the Vita (loader/modset.h), so its
label should say so. The main-menu buttons are pre-rendered images in
patch.obb's override/, and there is no font to draw a new label with -- so the
letters are cut from the other buttons instead: M and o from "More Games", d
from "Load Game", s from "Options", each recoloured to the Google Play button's
own text colour and laid on a blank copy of the Options button, which shares
its style. Game art never enters the repo; the build makes the images from the
builder's own OBB and puts them in the VPK, the same way it bundles the APK's
shaders and fonts.

    mods_button.py <patch.obb or its extracted folder> <out dir>

Writes ios_mm_gp_en.tga (normal) and ios_mm_gp2_en.tga (highlighted). The
loader serves them for every language's Google Play button.

Plain Python on purpose: no PIL or numpy for a build step.
"""
import os
import struct
import sys
import zipfile

W, H = 756, 106          # every main-menu button image
BG_X = 60                # a column of plain button background, left of any text
TEXT_X0, TEXT_X1 = 20, 735


def read_source(src, name):
    path = "override/" + name
    if os.path.isdir(src):
        with open(os.path.join(src, path), "rb") as f:
            return f.read()
    with zipfile.ZipFile(src) as z:
        return z.read(path)


def decode(data):
    """32-bit TGA, plain or run-length (both occur among the buttons) -> rows
    of [r, g, b, a], top row first."""
    idlen, cmtype, itype = data[0], data[1], data[2]
    w, h, bpp, desc = struct.unpack_from("<HHBB", data, 12)
    if cmtype != 0 or itype not in (2, 10) or bpp != 32 or (w, h) != (W, H):
        sys.exit("unexpected TGA format (type %d, %d bpp, %dx%d)" % (itype, bpp, w, h))
    pos = 18 + idlen
    if itype == 2:
        px = data[pos:pos + w * h * 4]
    else:
        px = bytearray()
        while len(px) < w * h * 4:
            head = data[pos]
            n = (head & 0x7F) + 1
            if head & 0x80:
                px += data[pos + 1:pos + 5] * n
                pos += 5
            else:
                px += data[pos + 1:pos + 1 + 4 * n]
                pos += 1 + 4 * n
    rows = [[[px[(y * w + x) * 4 + 2], px[(y * w + x) * 4 + 1],
              px[(y * w + x) * 4], px[(y * w + x) * 4 + 3]] for x in range(w)]
            for y in range(h)]
    if not desc & 0x20:          # bottom-up storage
        rows.reverse()
    return rows, desc


def encode(rows, desc):
    out = bytearray(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, W, H, 32, desc))
    order = rows if desc & 0x20 else rows[::-1]
    for row in order:
        for r, g, b, a in row:
            out += bytes((b, g, r, a))
    return bytes(out)


def diff(p, q):
    return sum(abs(p[i] - q[i]) for i in range(4))


def text_columns(img):
    """Runs of columns holding text, left to right."""
    cols = [x for x in range(TEXT_X0, TEXT_X1)
            if any(diff(img[y][x], img[y][BG_X]) > 60 for y in range(H))]
    runs, start = [], None
    for i, x in enumerate(cols):
        if start is None:
            start = x
        if i + 1 == len(cols) or cols[i + 1] != x + 1:
            runs.append((start, x))
            start = None
    return runs


def text_colour(img):
    best, colour = -1, None
    for y in range(H):
        for x in range(TEXT_X0, TEXT_X1):
            d = diff(img[y][x], img[y][BG_X])
            if d > best:
                best, colour = d, img[y][x]
    return colour


def glyph(img, index):
    """Coverage (0..1) of one letter, so it can be redrawn in another colour
    over another background -- "Load Game" is a different button style."""
    x0, x1 = text_columns(img)[index]
    t = text_colour(img)
    cov = []
    for y in range(H):
        b = img[y][BG_X]
        tb = [t[i] - b[i] for i in range(4)]
        n = max(sum(v * v for v in tb), 1)
        cov.append([min(max(sum((img[y][x][i] - b[i]) * tb[i] for i in range(4)) / n, 0.0), 1.0)
                    for x in range(x0, x1 + 1)])
    return cov


def build(src, suffix):
    def load(stem):
        return decode(read_source(src, "ios_mm_%s%s_en.tga" % (stem, suffix)))

    base, desc = load("opt")
    more, _ = load("mor")
    load_game, _ = load("loa")
    options, _ = load("opt")
    colour = text_colour(base)

    # Blank the label by repeating the plain background column across it; the
    # button is uniform horizontally apart from its rounded ends.
    img = [[list(row[BG_X]) if 200 <= x < 560 else list(row[x]) for x in range(W)]
           for row in base]

    letters = [glyph(more, 0), glyph(more, 1), glyph(load_game, 3), glyph(options, 5)]
    gaps = [12, 9, 9]                # as spaced in "More Games"
    width = sum(len(g[0]) for g in letters) + sum(gaps)
    x = (W - width) // 2
    for i, g in enumerate(letters):
        for y in range(H):
            for dx, c in enumerate(g[y]):
                p = img[y][x + dx]
                img[y][x + dx] = [int(p[k] * (1 - c) + colour[k] * c + 0.5) for k in range(4)]
        x += len(g[0]) + (gaps[i] if i < len(gaps) else 0)
    return encode(img, desc)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for suffix in ("", "2"):
        with open(os.path.join(out, "ios_mm_gp%s_en.tga" % suffix), "wb") as f:
            f.write(build(src, suffix))


if __name__ == "__main__":
    main()
