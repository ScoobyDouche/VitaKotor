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
#define MEMORY_NEWLIB_MB          160
// RAM vitaGL must LEAVE UNCLAIMED. It takes everything else at vglInitExtended,
// which runs before the game does, so anything we mean to allocate later has to
// be inside this number or it will not be there. It was 16 MB when newlib held
// 192; newlib now holds 160 and bigalloc wants up to 32 of the difference, so
// this is 16 + 32. Leave the two in step: shrinking newlib without raising this
// just hands the difference to vitaGL and the pool finds nothing.
#define MEMORY_VITAGL_THRESHOLD_MB 48

// Big-allocation pool (bigalloc.c). Everything at or above BIGALLOC_MIN_BYTES is
// served from segments of our own instead of newlib's arena, because mixing
// megabyte blocks with the game's thousands of small long-lived objects is what
// shreds the heap: log145 watched the largest servable block halve every ~150 s
// -- 32 MB down to 512 KB -- while 46 MB stayed free the whole time.
//
// The threshold is 256 KB, not 512 KB, because log142's fatal request was
// 286 KB. Traffic at that size is light (1458 in a 34-minute session), so the
// pool's best-fit walk costs nothing.
//
// This budget is TAKEN FROM newlib, not added: vitaGL claims everything except
// MEMORY_VITAGL_THRESHOLD_MB, so there is nothing spare. 160 + 32 is the old
// 192. Both halves are provisional -- the [big] log line reports peak live and
// fallback count, which is what says whether to move the line.
#define BIGALLOC_MIN_BYTES (256u * 1024u)
#define BIGALLOC_SEG_MB    8
#define BIGALLOC_MAX_SEGS  4

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

// Dress the boot screen in the game's own loading-screen art instead of a bare
// bar. The background comes from a load_*.tga inside patch.obb, which is a
// plain STORED zip entry and so readable before mount_obbs() runs. Set to 0 to
// go back to the untextured bar: the art path touches GL state (a texture, the
// fixed-function matrices) that the scissor-and-clear bar never did, so it is
// the first thing to rule out if a boot regresses.
#define LOADSCREEN_ART            1

// Bar geometry in the ART's own pixels, not loadscreen.gui's.
//
// The first attempt used PB_PROGRESS's extent from loadscreen.gui (LEFT 380,
// WIDTH 262 in its 1024x768 space) and sat visibly too wide. The groove the bar
// belongs in is painted into the load_*.tga itself, and measuring the two
// bright edge posts across all 97 of them puts it at x 437..586 -- 149 wide,
// not 262. Same centre, which is why it looked close but overhung by about 30
// pixels each side. Since we stretch the art over the whole framebuffer, the
// bar has to be anchored to the art, or the two cannot stay registered.
//
// Vertically the two sources agree, so this keeps loadscreen.gui's TOP 446 and
// HEIGHT 35 converted into art rows (x512/768).
#define ART_W                     1024
#define ART_H                     512
#define ART_BAR_X                 437
#define ART_BAR_W                 149
#define ART_BAR_Y                 297
#define ART_BAR_H                 23

// The rest of the screen, also in art pixels. loadscreen.gui's extents were the
// starting point but its horizontal figures do not survive the stretch (see the
// bar above), so these were set by composing the real assets against a capture
// of the game's own screen until they matched.
//
// The logo is width-anchored and sits on ART_LOGO_BOTTOM, which is just clear of
// the picture inset the art starts at row 164. Its file carries wide
// transparent margins, so the drawn quad uses the opaque bounding box.
#define ART_LOGO_W                300
#define ART_LOGO_BOTTOM           156
#define ART_LOAD_CX               517   // "LOADING", centred (LBL_LOADING)
#define ART_LOAD_Y                326
#define ART_HINT_CX               513   // the rotating line (LBL_HINT)
#define ART_HINT_Y                352
#define ART_HINT_W                590   // narrower than LBL_HINT's 748: matches
                                        // where the game's own screen wraps

// Seconds each line stays up. The screen freezes when the game takes over, at
// about 18s of a warm boot, so this is what decides how many are ever seen.
#define LOADSCREEN_HINT_SECONDS   6

// Assets read at boot. The .txi ships in the VPK for the game's own use; the
// atlas and the logo come out of patch.obb.
#define FONT_TXI_PATH             "app0:fonts/dialogfont16x16b.txi"
#define FONT_TGA_ENTRY            "override/dialogfont16x16b.tga"
#define LOGO_TGA_ENTRY            "override/and_main_logo.tga"

// How many times to dump the game's GL state after it takes over. The art can
// only draw while the loader owns GL outright; once the game starts issuing GL
// the loadscreen stops for good (see loadscreen.h). These probes record what
// was bound at that point, so a future attempt to keep drawing for the whole
// boot starts from measurements rather than guesses.
#define LOADSCREEN_PROBE_MAX      24
#define LOADSCREEN_PROBE_MS       2000

// Per-call GL trace budget. Every traced call is one sceIoWrite to the memory
// card, and log125/126 measured what that costs: gaps after a log line sit at a
// flat ~8-11ms floor regardless of WHICH line it was, and the two slowest gaps
// mod 64 are residues 0 and 1 in both runs -- exactly log.c's close/reopen
// boundary. So the logger, not the engine, sets the pace. Before the first draw
// (70.3s) both runs wrote ~4-5.4k lines, ~60% of startup, of which the per-call
// GL trace alone was 1958 lines / 21.0s (log125) and 3148 / 24.7s (log126).
// The budget is never exhausted before the menu, so a shipping build pays all of
// it. 0 disables the trace (GLLOG still ticks the loadscreen and emits one
// "silenced" line); raise it to 4000 to get the bring-up trace back.
#define GL_TRACE_LIMIT 0

// Log write buffering. Every log line used to be its own sceIoWrite to the
// memory card, measured at 8-11ms, which put the logger on the critical path of
// both startup and frames (logs 127/128: ~3370 gameplay lines per session, ~34s
// of ~175s of frame time, 18-19%). Batch lines into one buffer instead and write
// when it fills or when LOG_FLUSH_MS has elapsed, so a burst costs one card
// write rather than hundreds while a quiet period still reaches disk promptly.
//
// The time-based flush is what bounds the risk: on a hard hang (not a CPU fault,
// which has its own path) at most LOG_FLUSH_MS of log is unwritten. This project
// has chased several stalls, so that ceiling matters more than the last few
// syscalls. Set LOG_BUFFER_KB to 0 for the old unbuffered line-at-a-time
// behaviour.
// Diagnostic probe logging. The probes are how every bug in this port has been
// found, and they are also what fills the card: in log162, twelve periodic tags
// accounted for about 85% of 17,599 lines across 28 minutes, and that write
// traffic lands on exactly the loading and combat bursts that stutter.
//
// SHIPPED BUILDS KEEP THIS ON. The log is how players report faults: they post
// ux0:data/kotor/log.txt and that is what makes a bug fixable by someone who
// was not holding the console. A quiet release trades away the entire support
// path to buy back some card I/O, which is a bad trade and not one to make
// again without asking.
//
// Set to 0 only to measure what the logging itself costs, or for a build made
// for one person who is chasing framerate. When it is 0, lines are dropped
// before they are even formatted -- the tag is a literal prefix of the format
// string, so the test is a few byte compares and no vsnprintf, no lock, no
// card I/O -- and anything whose format mentions a failure, an error or a
// warning is kept regardless, as is everything written in panic mode.
#define LOG_DIAGNOSTICS 1

#define LOG_BUFFER_KB  8
#define LOG_FLUSH_MS   1000

// Heap tracing. gl_patch.c arms it at the last GL cap query (entering engine
// setup) to catch an UNBOUNDED alloc loop during bring-up, and never disarms it,
// so it runs for the whole session. MEM_TRACE is already throttled to every
// 1024th allocation -- but KOTOR allocates ~2.3M times in a 700s session, so the
// heartbeat alone emitted 2249 lines in log119, the single largest log source in
// gameplay (26% of all lines, ~54s of frame time). Off for release; set to 1 to
// hunt an allocation loop again.
#define MEM_TRACE_ENABLE 0

// glGet* value trace. The diagnostic value is all in init (~18 cap queries);
// steady-state values are noise, and log119 still spent 231 lines on them. The
// UNWRITTEN case stays unconditional either way -- it always signals a real
// vitaGL gap. Raise to 256 to restore the bring-up trace.
#define GLGET_TRACE_LIMIT 0

// Skip glBindTexture calls that rebind what is already bound. log120 measured
// 1.008 texture binds per draw call, so nearly all of them are redundant.
// Set to 0 if textures ever look wrong, to rule this out.
#define GL_FILTER_REDUNDANT_BINDS 1

// Skip glUseProgram calls that re-select the program already current. vitaGL's
// glUseProgram does no GXM work but marks every uniform dirty, so a redundant
// one costs a full uniform re-upload (u_boneMatrices[51] included) on the next
// draw. The Undercity makes ~43 of these a frame across ten programs total.
// MEASURED 0% IN log151: the game alternates programs genuinely and essentially
// never re-selects the current one, so this saves nothing in practice. Kept
// because it is one comparison and correct, not because it earned its place --
// the real cost is that ~2700 GENUINE switches per window each force a full
// uniform re-upload, and that cannot be filtered away from here.
// Set to 0 if lighting or skinning ever looks stale, to rule this out.
#define GL_FILTER_REDUNDANT_PROGS 1

// Pad NPOT texture widths to a multiple of 8 on upload. Added to prove the
// background-shear theory, and it did. The pad leaves the texture wider than
// the game believes, so a quad sampling u across [0,1] also covers the pad;
// it now RESAMPLES the row onto the padded width rather than extending the
// last column, so [0,1] still spans the whole picture at any size.
//
// That resample was aimed at the oversized minimap and the fog panel in the
// character screens, and it did NOT fix them -- log156 shows 81 resampled
// uploads (2x2, 4x4, 90x70, 756x106, 860x478) with both boxes unchanged on
// screen. Keep the resample anyway: it is strictly more correct than the smear
// it replaced, and the tiny gradients it was mangling were real. But the boxes
// are NOT the upload pad, so do not come back here for them. The real fix for
// the shear this exists to mask is strided texture init in vitaGL.
#define GL_NPOT_WIDTH_PAD 1

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

// Scale GUI images that never went through ScaleExtentForResolution.
//
// log157 asked, per widget, whether it had been scaled, and every image at or
// above 200x200 came back NO -- while 138 other widgets in the same screen had
// been scaled normally. Two of the three are the pillarbox wings, which arrive
// at exactly the screen height (217x544 at x=-100 and x=843) and are already in
// device pixels; shrinking those would be wrong, so anything at or above the
// screen height is left alone. The third is a 238x238 image that is neither a
// texture size nor a device-space number, and 238 x 0.7083 is 169: an element
// left at authored size inside a frame the game scaled by 544/768 is 1.41x too
// big for it, which is exactly how far the minimap overflows its frame.
//
// TESTED AND DISPROVED (log158). The rescale fired exactly once, on precisely
// the widget it was aimed at -- "autoscaled self=0x85b42738 238x238 by x0.7083"
// -- and the minimap was unchanged on screen. So either that widget is not the
// minimap, or its size was never the problem. Either way the extent path is now
// finished as an explanation for these boxes: scaling is applied correctly to
// the 138 widgets that get it, and forcing it onto the ones that skip it fixes
// nothing.
//
// Left at 0. It is a speculative mutation of widget geometry with no evidence
// behind it any more, and shipping one of those is worse than the bug.
//
// Next suspect is blending, not geometry: the haze bands line up with the UI
// slots and read like additive overlays drawn opaque, and KOTOR stores
// per-texture blend modes in .txi files -- of which log157 shows a great many
// missing.
#define GUI_AUTOSCALE_UNSCALED_IMAGES 0

// Spatial audio. All four FMOD 3D entry points -- set3DAttributes,
// set3DMinMaxDistance, set3DListenerAttributes, set3DOcclusion -- were
// fmod_stub, so no positional sound ever attenuated with distance or panned:
// rushing water across the level played at the same level as water underfoot,
// and footsteps and dialogue sat under the ambience. log155 marks 418 of 617
// logged sounds FMOD_3D, so this is most of the mix, not an edge case.
//
// Implements FMOD Ex's default inverse rolloff (gain = mindistance/distance,
// flat inside mindistance, floored -- not silenced -- past maxdistance) plus a
// pan projected onto the listener's right vector. 2D sounds are untouched by
// design: music, UI and the ambient bed carry no position and play flat.
// Set to 0 to go back to every source at full volume, dead centre.
#define AUDIO_3D_ATTENUATION 1

// Handedness of the 3D listener basis. FMOD Ex is left-handed by default and
// only switches when the game passes FMOD_INIT_3D_RIGHTHANDED to System::init.
// The two conventions produce exactly opposite right vectors, i.e. a mirrored
// stereo image -- inaudible as a defect, wrong every time. Left-handed takes
// up x forward, right-handed forward x up. The first listener update prints the
// resulting basis so it can be checked against known geometry on hardware.
// Set to 1 if positional audio comes out mirrored.
#define AUDIO_3D_RIGHTHANDED 0

// Upload textures as 16-bit instead of 32-bit.
//
// The live-texture census (log161) is emphatic that nothing leaks: 1,014,190 KB
// uploaded against 920,002 KB released over 28 minutes, balancing to the byte.
// The problem is that the working set does not FIT -- it peaks at 102,639 KB
// against the Vita's fixed 96 MB of CDRAM -- so vitaGL evicts and re-uploads
// continuously, runs at a few hundred KB free, and the picture starts tearing.
// VRAM cannot be raised to meet it; MEMORY_VITAGL_THRESHOLD_MB governs system
// RAM, not CDRAM. The only lever is making the textures smaller.
//
// RGBA8 -> RGBA4444 and RGB8 -> RGB565 halves the footprint to roughly 51 MB,
// which fits with room to spare. Ordered 4x4 Bayer dithering keeps the banding
// down; without it, gradients and skies posterise badly at 4 bits a channel.
//
// Render targets and cube maps are left alone: an FBO's attachment format is
// not ours to change, and the environment map is 64x64x6 and not worth the
// risk. Textures we convert are remembered per id so glTexSubImage2D can
// match, since a byte-typed sub-upload into a 4444 texture would corrupt it.
// Set to 0 to go back to full-precision uploads.
#define GL_TEX16_CONVERT 1

