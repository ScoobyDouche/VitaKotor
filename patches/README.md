# Patches to dependencies

vitaGL is linked **statically** into `eboot.bin`, so a change to it is a change
to the shipped binary with nothing in this repository to show for it. Anything
in here is therefore part of the build, not an optional extra: a VPK built
without these patches is not the VPK that was released.

## `vitagl-vbo-offset-64k.patch`

Against vitaGL at `38d2f97` — the commit SETUP.md section 4 pins.

`SceGxmVertexAttribute::offset` is 16 bits wide. vitaGL was assigning a 32-bit
`vertex_attrib_offsets[]` entry straight into it, so any draw pointing more than
64 KB into a vertex buffer had its offset silently truncated mod 64 KB and the
GPU fetched every attribute from the wrong vertex. KOTOR packs many meshes into
one buffer and draws each from its own slice, so it crosses that line routinely
on larger models — this was the long-running suspect behind the geometry
spikes and tearing.

The patch rebases instead of truncating: it finds the lowest enabled attribute
offset, folds it into the stream pointer, and stores only the intra-vertex
remainder, which always fits. Draws whose offsets already fit are left
byte-for-byte as they were.

Shipped in v0.1.12.

### Applying it

```bash
cd ~/vitadev/vitaGL
git checkout 38d2f97
git apply /path/to/VitaKotor/patches/vitagl-vbo-offset-64k.patch
make clean && make LOG_ERRORS=1 -j$(nproc) && make install
```

`LOG_ERRORS=1` is not optional and `make clean` is not either — see SETUP.md
section 4 for why.
