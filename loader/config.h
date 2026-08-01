/* config.h -- compile-time constants for the KOTOR Vita loader */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Where the game's writable data / logs live
#define DATA_PATH        "ux0:data/kotor"
#define LOG_PATH         DATA_PATH "/log.txt"

// The Android shared objects we so-load (copied out of the APK by the user)
#define SO_PATH          DATA_PATH "/libKOTOR.so"
#define ANDROID_PORT_SO  DATA_PATH "/libandroid_port.so"
// Compression libs the game/companion NEED (mz_zip_reader_* / LzmaUncompress).
// Loaded before the companion so those imports resolve cross-module.
#define MINIZ_SO         DATA_PATH "/libminiz.so"
#define LZMA_SO          DATA_PATH "/libLzmaLib.so"

// OBB game-data archives (ZIPs read by the companion's ObbFile/miniz). The user
// copies main.<v>.com.aspyr.swkotor.obb / patch.<v>...obb here, renamed. The
// game normally mounts these via Java; we drive mountObb/mountPatchObb instead.
#define OBB_MAIN_PATH    DATA_PATH "/main.obb"
#define OBB_PATCH_PATH   DATA_PATH "/patch.obb"

// Base addresses the modules are mapped at (distinct so they don't overlap).
#define LOAD_ADDRESS              0x98000000  // libKOTOR.so (main, ~6 MB)
#define ANDROID_PORT_LOAD_ADDRESS 0x90000000  // libandroid_port.so (~900 KB)
// In the proven-good high-user range (0x8Cxxxxxx collides with the 192MB heap);
// slot both between libandroid_port (0x90000000) and libKOTOR (0x98000000).
#define MINIZ_LOAD_ADDRESS        0x92000000  // libminiz.so (~63 KB)
#define LZMA_LOAD_ADDRESS         0x94000000  // libLzmaLib.so (~22 KB)

// Font metrics (.txi) injection. Aspyr ships the override font TGAs without their
// companion .txi glyph metrics, so CAurFontInfo never populates -> fontInfo stays
// NULL -> every GUI text path null-derefs (WrapStrings, Draw, GetIdealPixelHeight).
// That is what makes the menu render with no text.
//
// We serve VPK-bundled .txi files, but the DELIVERY SHAPE matters. AurResGet has two
// exits that build the same 28-byte object with different layouts, and
// AurResGetNextLine dispatches on [+0]:
//
//   OBB hit  (+0x40e3dc): [+0]=0      [+12]=data [+16]=size [+20]=8192   *size=len
//   fallback (+0x40e4d0): [+0]=RWops* [+12]=0    [+16]=0    [+20]=500000 *size=1
//
//   [+0]==0 -> in-memory scanner, bounds-checked against [+16] (+0x40e8ca)
//   [+0]!=0 -> streaming scanner (+0x40e920), UNBOUNDED
//
// Merely enabling the loose-file fallback (flag=0) lands on the streaming scanner,
// which after the first line resumes at index bytes_read+1 inside a 500 KB buffer
// holding ~11 KB of file and walks uninitialised heap hunting CR/LF -> DATA_ABORT
// during engine init (log41 died at GL call #62 vs 4000+ in log36-40). So instead we
// let the game build AND REGISTER the fallback object, then convert it in place to
// the memory-backed shape. See AurResGet_hook in main.c.
#define FONT_TXI_MEMORY_INJECT 1

// Screen geometry
#define SCREEN_W         960
#define SCREEN_H         544

// Heap / memory budgets (MB). 192 MB fits within the extended-memory partition
// granted by ATTRIBUTE2=12 (see CMakeLists.txt). Requesting more (e.g. 256)
// without that grant makes the CRT heap init fail -> crash in _sbrk_r before main.
#define MEMORY_NEWLIB_MB          192
#define MEMORY_VITAGL_THRESHOLD_MB 16

// Multisampling. We shipped SCE_GXM_MULTISAMPLE_4X, which makes the GPU shade
// and resolve 4x the fragments at 960x544 -- on a Vita that is a luxury, and it
// costs most in exactly the geometry-heavy scenes that stutter (log118: slow
// windows averaged 30737 drawElements vs 7400 in fast ones).
//   SCE_GXM_MULTISAMPLE_NONE  fastest, aliased edges
//   SCE_GXM_MULTISAMPLE_2X    middle ground
//   SCE_GXM_MULTISAMPLE_4X    original, prettiest, slowest
#define GL_MSAA_MODE SCE_GXM_MULTISAMPLE_NONE

// ---- Archive mount speed & feedback ----------------------------------------
// Mounting main.obb reads a ~46-byte local header at each of ~16k entries,
// scattered across 1.75 GB. Measured from the real archive: mean gap between
// consecutive headers 107 KB, median 38 KB. Read-ahead was tried and DISPROVED
// (log122): a 32 KB block buffer moved 373 MB to eliminate 30% of the reads and
// made the mount slower, 95s -> 103s. Sparse access defeats prefetching.
//
// What works instead is that the mount reads the same bytes every boot, so we
// record them once and replay them from a small file afterwards. See
// obb_index.h. First boot is unchanged; later boots should skip the scattered
// reads. Delete ux0:data/kotor/main.obb.idx to force a re-record.
#define OBB_INDEX_CACHE      1
#define OBB_INDEX_BUDGET_KB  4096   // cap on recorded bytes (~735 KB expected)
#define OBB_INDEX_MAX_RANGE  4096   // ignore reads bigger than this: asset data,
                                    // not metadata, and it would bloat the cache

// Read archives with sceIoPread instead of fseek+fread. One syscall per read
// rather than two, and no newlib buffer to invalidate -- which matters because
// every archive read is preceded by a seek to the virtual handle's position.
#define OBB_USE_PREAD        1

// Progress bar for startup. The game draws NOTHING until its first draw call --
// log124 measured that at 69.3s, later even than the "Main Menu" analytics
// string at 56.1s -- so without this the console looks hung for over a minute.
// The bar spans loader start to that first draw, estimating from how long the
// previous boot took (saved in ux0:data/kotor/startup.tim). Warm and cold
// archive-cache timings are tracked separately; they differ by about a minute.
#define LOADSCREEN_ENABLE         1
#define LOADSCREEN_REDRAW_MS      100
#define LOADSCREEN_DEFAULT_WARM_S 70    // first-ever run with a built .idx cache
#define LOADSCREEN_DEFAULT_COLD_S 135   // first-ever run, building the cache

// Skip glBindTexture calls that rebind what is already bound. log120 measured
// 1.008 texture binds per draw call, so nearly all of them are redundant.
// Set to 0 if textures ever look wrong, to rule this out.
#define GL_FILTER_REDUNDANT_BINDS 1

// Skinning bisect: force the ubershader's `#define USE_SKIN 1` to 0 in
// glShaderSource, bypassing kotor.vert's dynamic uniform-array read
// (u_boneMatrices[indices.x]) while leaving everything else identical.
// Diagnostic for the exploding-character bug -- characters render in BIND POSE
// (static but correctly shaped) while enabled. See gl_patch.c for the full
// rationale. Set to 0 to restore real skinning.
#define SKIN_BISECT_DISABLE 0

// Skinning bone-index rounding: rewrite kotor.vert's
//   ivec4 indices = ivec4(clamp(3.0 * a_matrixIndices, 0.0, 50.0));
// to round-to-nearest instead of truncating. DISPROVED (log79): the rewrite was
// confirmed applied to every skinned vertex shader ("SKIN FIX sh=N: 1 site") and
// the characters exploded exactly as before, so fp16 demotion of a_matrixIndices
// was never the problem. Left in place, off, because it costs nothing to re-arm.
// The real cause was vitaGL truncating >64 KB VBO attribute offsets into GXM's
// 16-bit SceGxmVertexAttribute::offset -- fixed in vitaGL, see
#define SKIN_INDEX_ROUND_FIX 0

#endif
