# The post-mount startup window is the logger, not the engine

**Status:** root cause identified from logs 125/126; first fix applied
(`GL_TRACE_LIMIT` 4000 -> 0), **not yet verified on hardware**.
**Date:** 2026-08-01

## Problem

After the archive-mount work cut startup from ~142s to ~54-63s, a large window
remained between the mount finishing and the first drawn frame. It had been
recorded as "25s -> 69s, almost no I/O, ~1400 GL trace lines, engine init".

Both halves of that description were wrong.

## The premise was an instrumentation artifact

`loader/main.c:257` sets `g_io_trace = 0` on the line immediately after
`mountPatchObb` returns. That flag gates `fopen` success logging
(`dynlib.c:458`), `fseek` (`:508`), `ftell` (`:542`) and the plain-`fread`
heartbeat (`:588`). So the window characterised as having "almost no I/O" begins
at the exact instruction that switches I/O logging off. The only I/O evidence
that survives past the mount is the archive heartbeat every 8192 reads
(`dynlib.c:574`) and `fopen` failures.

The window boundaries were also off: on a warm boot (log125) `mountPatchObb`
returns at **10.7s**, not 25s, and the first draw is at 70.3s. The real window is
~60s.

## What the watchdog already recorded

`watchdog_thread` (`main.c:115`) samples `runClocks` every 3s and is started
(`main.c:2234`) before the game thread (`:2241`), so it covers the whole boot.
Its samples split the window cleanly in two:

| Phase | Window | Game thread | dClk per 3.0s interval | Reading |
|---|---|---|---|---|
| A | 10.7 - 26s | `RUNNING` | 2.7M - 3.1M | ~95% CPU-bound, real work |
| B | 26 - 70.3s | `WAITING`, `waitType=0x10`, `waitId=0x1065b` unchanged | 0.17M - 0.8M | ~85% blocked |

Both registered workers are idle for the whole of phase B (`runClk` 715 and 675
microseconds, `<< FROZEN`). The main thread is blocked, the workers are doing
nothing, and the console is largely asleep for 44 seconds.

Phase A is genuine: the `[io]` heartbeats show ~74k uncached archive reads
between 13.6s and 25s, with `cache hits` frozen at 31788 — the `.idx` replay
cache serves the mount and then stops contributing.

## Root cause of phase B

Attributing wall-clock gaps to the preceding log line's tag gives a flat floor
after **every** tag, regardless of what the line was:

```
PHASE B 26-70.3s  (log125, total 44.3s, 3408 lines)
  21.35s  48.2%  n=1953  mean=10.93ms  after [GL#N]
   4.70s  10.6%  n= 322  mean=14.61ms  after [FS]
   2.83s   6.4%  n= 275  mean=10.27ms  after [GLSRC
   1.62s   3.6%  n= 144  mean=11.23ms  after [gui]
   1.61s   3.6%  n= 144  mean=11.18ms  after [SDL]
```

A cost that does not depend on the content of the line is the cost of *writing*
the line. Two independent checks confirm it:

1. **84% of the window is sub-30ms gaps** in both runs — 37.1s of 44.3s
   (log125), 37.2s of 44.3s (log126). No single stall; the top 25 gaps together
   are only 17s.
2. **Period-64 flush signature.** `log.c:42` closes and reopens the file every
   `LOG_FLUSH_EVERY` = 64 lines. Bucketing phase-B gaps by line index mod 64,
   the two slowest residues are **0 and 1 in both logs independently**, at 1.5x
   baseline. That is the close/reopen boundary showing through, which only
   happens if the logger is on the critical path.

Total, before the first draw:

| Log | Lines | Per-line floor | Implied logging cost | Share of 70.3s |
|---|---|---|---|---|
| 125 | 4082 | 10.7ms | 43.9s | 62% |
| 126 | 5363 | 7.9ms | 42.2s | 60% |

The per-call GL trace alone is 1958 lines / 21.0s (log125) and 3148 / 24.7s
(log126). `GL_TRACE_LIMIT` was 4000 and is never exhausted before the menu, so a
shipping build pays all of it.

## Fix applied

`GL_TRACE_LIMIT` moved from `gl_patch.c` to `config.h` and set to 0. The `GLLOG`
macro handles 0 without any code change: `_s < 0` never fires, `_s == 0` emits a
single "silenced" line, and `loadscreen_tick()` stays outside the conditional so
the loading bar still updates. Verified on the host — 5000 calls produce exactly
1 log line and 5000 ticks. Re-arm by setting it back to 4000.

Expected saving ~21-25s of a 70.3s startup. **Unverified on hardware.**

## Next levers, in order

1. **Buffer the log.** Every line is currently one `sceIoWrite` syscall
   (`log.c:41`). A userland buffer would cut the floor for *all* categories
   rather than deleting diagnostics, and the crash path already calls
   `log_flush()`, so fault dumps would stay complete. Two cautions: `log_printf`
   is called from several threads and has no lock today (the kernel currently
   serialises whole lines for us), and the crash handler must not deadlock on
   whatever lock is added.
2. Remaining categories, at the measured floor: `[FS]` 400 lines, `[snd?]` 294,
   `[GLSRC` 275, `[res]` 193, `[gui]` 154.
3. **Phase A (10.7-26s) is untouched by any of this** — it is 74k real archive
   reads with the replay cache no longer hitting. That is the next genuine
   engine-side target once the logging noise is out of the measurement.

## Method note

Every number here came from logs already sitting in `~/Downloads/vitalogs/`.
No hardware round trip was needed. Re-measure with the trace off before trusting
any timing in the old logs: at ~60% overhead they measure the logger.
