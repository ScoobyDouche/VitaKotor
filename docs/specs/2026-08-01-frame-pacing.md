# Frame pacing: what actually causes the dropoffs

**Status:** analysis of logs 117/119/120; two log sources disabled, effect
projected but **not yet verified on hardware**.
**Date:** 2026-08-01

Companion to `2026-08-01-startup-logging-cost.md`, which found the loader's own
logging was ~60% of startup. The same question for gameplay: is the stutter real,
or are we measuring the logger again?

Answer: **both, and the split matters.**

## Frame time is dominated by log lines, but not caused by them

Per 120-frame window, correlating frame time against each counter:

| Log | log lines | clears | drawElements | drawArrays |
|---|---|---|---|---|
| 117 | **+0.889** | +0.226 | +0.273 | -0.174 |
| 119 | **+0.927** | +0.365 | +0.322 | +0.253 |
| 120 | **+0.987** | +0.342 | +0.218 | -0.185 |

Regressing frame time on lines per frame gives `frame_ms = 30-38 + 24 * lines`.
The 24ms/line slope is **not** a per-line cost — the startup analysis measured the
real write cost at 8-11ms. The excess is confounding: the windows that log most
are the windows loading assets, and the loading is itself the work. So the slope
brackets rather than measures:

| | log117 | log119 | log120 |
|---|---|---|---|
| logging at 10ms/line (true write cost) | | 80s of 531s = **15%** | |
| logging at 24ms/line (regression slope) | | 191s of 531s = 36% | |

Take 15% as the defensible figure and 36% as the ceiling.

The useful number is the **intercept** — frame time with zero logging:

| Log | intercept | implied baseline | observed median |
|---|---|---|---|
| 117 | 38.5ms | 26.0 fps | 26 fps |
| 119 | 31.9ms | 31.3 fps | 34 fps |
| 120 | 29.6ms | 33.8 fps | 27 fps |

**The game runs at ~26-38 fps even with logging entirely removed.** 60 fps is not
one tuning step away.

## The dropoffs are real work, not logging

log119's worst window is 120 frames in **60.1s** (2.0 fps) carrying 2446 log
lines. Removing all of them at the true 10ms cost gives 3.4 fps. The window is a
GUI/model load — `[SDL]`=456, `[GL]`=437, `[model]`=280, `[vtx]`=207 — i.e. ~36s
of genuine synchronous asset loading on the main thread while the swap chain
keeps ticking. That is a loading event, not a frame-pacing bug, and no amount of
log trimming fixes it.

Typical dropoffs (p10 ≈ 12 fps) carry only 90-150 lines per window, so logging is
~10% of those. Also real work.

## Changes made

Both reversible from `config.h`, reusing the gates the codebase already had.

1. **`MEM_TRACE_ENABLE 0`** — `gl_patch.c` armed the heap trace at the last GL cap
   query and never disarmed it. `MEM_TRACE` is throttled to every 1024th
   allocation, but KOTOR allocates ~2.3M times per 700s session, so the heartbeat
   alone was 2249 lines in log119 — **26% of all gameplay log lines**, the single
   largest source. Verified dropped from the binary: the `arming heap trace`
   string is gone, and nothing else writes `g_mem_trace`.
2. **`GLGET_TRACE_LIMIT 0`** (was 256) — 231 lines of steady-state getter values.
   The UNWRITTEN case stays unconditional; it always signals a real vitaGL gap.

Projected from the logs, applying only the true 10ms write cost:

| Log | median fps | p10 (the dropoffs) | worst |
|---|---|---|---|
| 117 | 25.6 -> 27.4 | 11.6 -> 12.4 | 2.8 -> 3.0 |
| 119 | 34.0 -> 38.3 | 12.1 -> 14.0 | 2.0 -> 2.3 |
| 120 | 27.4 -> 30.3 | 16.7 -> 19.1 | 1.8 -> 2.1 |

Worth having and free, but **this does not fix the dropoffs**, and the honest
expectation is +2-4 fps median.

## Measured and rejected

**Negative-caching the `.ezf` probes.** Before every texture load the game probes
a file that never exists (`swpc_tex_gui.ezf` 381 times in each log), then opens
the real `.erf`. Tempting. But bracketing the failed open between its two
surrounding log lines puts it at **9.3ms median, of which 8.2ms is the log line
itself** — the failed `sceIoOpen` costs ~1ms. Caching all of them would save
~0.4s across a 700s session. Not worth the behaviour change.

Note the MISS count is capped: `sdl_patch.c:353` stops at 600 lines, which is why
all three logs report exactly 600. The true count is higher and unknown.

## Next

1. **`clientArrayDraws` vs `vboDraws` has never actually been logged.** The
   counter exists (`gl_patch.c`, `per-window:` line) but appears in none of
   logs 117-120 — it was added after they were captured. If most draws are
   client-side arrays, vitaGL copies vertex data into its circular pool on every
   draw, which would explain a ~30 fps ceiling far better than fill rate. **The
   first run of the current build produces this number for the first time.**
   Do that before tuning anything else.
2. `clears` run at 160-205 per frame (max 683). Weak correlation with frame time,
   but on a tile-based GPU a mid-scene clear can force a resolve. Worth a look
   once the draw-source split is known.
3. Remaining log sources, at ~10ms each: `[GL]` 794/log, `[res]` 603, `[gui]` 524,
   `[SDL]` 498, `[FS]` 458, `[snd?]` 374, `[model]` 359. Buffering `log.c` writes
   would cut all of them at once and keep the diagnostics, at the cost of adding
   a lock to a path several threads call — see the startup spec.
   **Done — see Outcome below.**

## Outcome: buffering worked, and it separates the two problems

`LOG_BUFFER_KB 8` / `LOG_FLUSH_MS 1000`, measured on hardware in log129 against
log127 (the fair comparison — both ~210-220 draws/frame; log128 was a lighter
scene at 132 and is not comparable):

| | v0.1.8 (log127) | buffered (log129) |
|---|---|---|
| per-log-line cost | 11.63ms | **0.27ms** |
| lines landing in <1ms | 1% | **61%** |
| logging share of frame time | 19% | **0.7%** |
| worst window | 16.0 fps | **20.2 fps** |
| slowest 10% | 19.9 fps | **22.1 fps** |
| median | 31.0 fps | 31.1 fps |
| startup to first frame | 43.5s | **28.4s** |

Ordering is intact: 0 out-of-order timestamps across 5196 lines, so the mutex is
correctly serialising the shared buffer across threads.

**The important result is the median NOT moving.** Content-normalised cost per
draw call is 166.1us before and 166.2us after — identical. The logging tax was
concentrated in the burst windows, not spread across every frame, so removing it
flattens the dips and leaves the ceiling untouched. The remaining ~31 fps median
and the 38.7 fps cap are now pure rendering cost, with the instrumentation out of
the measurement for the first time. Everything measured from here is the game.

The `ms/logline` regression slope fell from 13.91 to 4.27, confirming log volume
no longer drives frame time; the residual is the old confound, that log-heavy
windows are loading-heavy windows.

**Untested:** `log_panic()` has never fired on hardware — no fault occurred in
log129. It is verified by construction and by the host test, not by a real crash.
`LOG_BUFFER_KB 0` restores the old line-at-a-time behaviour exactly.

## Method note

All of this came from logs already in `~/Downloads/vitalogs/`. The pattern worth
keeping: **check whether the instrumentation explains the measurement before
believing the measurement.** It did for startup, it partly did here, and it
killed the `.ezf` theory.
