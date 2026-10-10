/* log.c -- file logger to ux0:data/kotor/log.txt (see log.h) */

#include <vitasdk.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "threads.h"

// Originally every line did its own open/append/close so a hard crash could not
// lose output. That is three memory-card syscalls per line; keeping one fd open
// and closing periodically cut it to one write per line. That was still too
// much: logs 125-128 measured a line at 8-11ms, which made the logger ~60% of
// startup and ~19% of gameplay frame time -- the log was pacing the game.
//
// So lines now accumulate in a buffer. That fixed the per-line cost but left a
// once-a-second one: the time-based flush wrote, closed and later reopened the
// file on whichever thread logged next, holding the log lock throughout, so
// any thread that logged meanwhile -- the game thread included -- waited on the
// card. In logs 226/229/230 about half of all 80-200 ms frames had that flush
// inside them, against 6% for the same windows shifted half a second.
//
// Now a writer thread owns the card I/O. Lines are copied into the active
// buffer under the lock and nothing more; every LOG_FLUSH_MS (or sooner, once
// the buffer is half full) the writer swaps buffers and writes the full one out
// with only the I/O lock held. A logging thread touches the card only when the
// writer has fallen a whole buffer behind.
//
// Two invariants matter more than the speed:
//   * A CPU fault must never deadlock on the log. crash.c calls log_panic()
//     before it writes anything; after that every line bypasses both the buffer
//     and the lock and goes straight to the card, as it did originally.
//   * A hard hang must not swallow the last of the log. The writer keeps
//     flushing every LOG_FLUSH_MS even when the thread that hung holds nothing.
#define LOG_BUF_BYTES (LOG_BUFFER_KB * 1024)

static SceUID g_log_fd = -1;

#if LOG_BUF_BYTES > 0
// Lock order: g_log_mtx (the active buffer) before g_io_mtx (the fd and the
// buffer being written out). Nothing takes them the other way round.
static char     g_bufs[2][LOG_BUF_BYTES];
static int      g_cur;          // index of the buffer lines go into
static int      g_buf_n;        // bytes used in g_bufs[g_cur]
static uint64_t g_last_flush;   // only used until the writer is up
static SceUID   g_log_mtx = -1;
static SceUID   g_io_mtx = -1;
static SceUID   g_wake = -1;    // writer's semaphore: half full, or timeout
static volatile int g_writer_up;
static int      g_kicked;       // wake already signalled for this buffer
static volatile int g_panic;

// stats, reported by the writer once a minute
static unsigned g_st_writes, g_st_kb, g_st_sync;
static uint32_t g_st_max_us;

// Lock only when there is a mutex and we are not handling a fault. Before
// log_init (or if mutex creation failed) there is a single thread anyway.
static inline int log_lock(void) {
  if (g_panic || g_log_mtx < 0) return 0;
  return sceKernelLockMutex(g_log_mtx, 1, NULL) >= 0;
}
static inline void log_unlock(int held) {
  if (held) sceKernelUnlockMutex(g_log_mtx, 1);
}
static inline int io_lock(void) {
  if (g_panic || g_io_mtx < 0) return 0;
  return sceKernelLockMutex(g_io_mtx, 1, NULL) >= 0;
}
static inline void io_unlock(int held) {
  if (held) sceKernelUnlockMutex(g_io_mtx, 1);
}
#endif

static void log_open_if_needed(void) {
  if (g_log_fd < 0)
    g_log_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
}

static void log_write_direct(const char *buf, int len) {
  log_open_if_needed();
  if (g_log_fd >= 0)
    sceIoWrite(g_log_fd, buf, len);
}

static void log_close(void) {
  if (g_log_fd >= 0) {
    sceIoClose(g_log_fd);
    g_log_fd = -1;
  }
}

#if LOG_BUF_BYTES > 0
// Write the active buffer out on the calling thread. Caller holds both locks,
// or is panicking.
static void log_drain(void) {
  if (g_buf_n <= 0) return;
  log_write_direct(g_bufs[g_cur], g_buf_n);
  g_buf_n = 0;
  g_kicked = 0;
}

static int log_writer(SceSize args, void *argp) {
  (void)args; (void)argp;
  uint64_t last_report = sceKernelGetProcessTimeWide();
  while (!g_panic) {
    SceUInt timeout = LOG_FLUSH_MS * 1000;
    sceKernelWaitSema(g_wake, 1, &timeout);
    if (g_panic) break;

    sceKernelLockMutex(g_log_mtx, 1, NULL);
    int n = g_buf_n;
    if (n <= 0) {
      sceKernelUnlockMutex(g_log_mtx, 1);
    } else {
      // Take the I/O lock before letting go of the buffer, so a logging thread
      // that overflows the new buffer cannot write it out ahead of this one.
      sceKernelLockMutex(g_io_mtx, 1, NULL);
      const char *out = g_bufs[g_cur];
      g_cur ^= 1;
      g_buf_n = 0;
      g_kicked = 0;
      sceKernelUnlockMutex(g_log_mtx, 1);

      uint64_t t0 = sceKernelGetProcessTimeWide();
      log_write_direct(out, n);
      log_close();   // durable on the card, as the inline flush was
      uint32_t us = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
      sceKernelUnlockMutex(g_io_mtx, 1);

      g_st_writes++;
      g_st_kb += (unsigned)(n + 512) / 1024;
      if (us > g_st_max_us) g_st_max_us = us;
    }

    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - last_report >= 60ULL * 1000000ULL && g_st_writes) {
      log_printf("[log] writer: %u writes, %u KB, max %.1f ms; %u overflow writes on a logging thread",
                 g_st_writes, g_st_kb, g_st_max_us / 1000.0f, g_st_sync);
      g_st_writes = g_st_kb = 0;
      g_st_max_us = 0;
      last_report = now;
    }
  }
  return sceKernelExitDeleteThread(0);
}
#endif

void log_flush(void) {
#if LOG_BUF_BYTES > 0
  int held = log_lock();
  int io = io_lock();
  log_drain();
  // Close inside the lock: another thread reopening/writing g_log_fd while we
  // close it would write to a stale descriptor and lose the line.
  log_close();
  io_unlock(io);
  log_unlock(held);
#else
  log_close();
#endif
}

void log_panic(void) {
#if LOG_BUF_BYTES > 0
  // Set before draining: from here on nothing takes the log lock, so a fault
  // that happened while another thread held it cannot deadlock us.
  g_panic = 1;
  // Let a write already in flight finish first so the dump lands after it,
  // but never wait long: the faulting thread may be the one holding it. The
  // lock is never released, which keeps the writer off the fd for good.
  if (g_io_mtx >= 0) {
    SceUInt timeout = 300 * 1000;
    sceKernelLockMutex(g_io_mtx, 1, &timeout);
  }
  log_drain();          // keep the crash dump ordered after earlier lines
#endif
}

/* First LOG_JNI_BUDGET lines only; see LOG_JNI in log.h. */
#define LOG_JNI_BUDGET 3000
int log_jni_enabled(void) {
  static int budget = LOG_JNI_BUDGET;
  if (budget <= 0) return 0;
  if (--budget == 0) {
    log_printf("[JNI] trace silenced after %d lines (steady state)", LOG_JNI_BUDGET);
    return 0;
  }
  return 1;
}

/* Retired probes: tags whose bug is closed, dropped in every build. Their
 * hooks stay (several do real work besides logging); only the lines go.
 *   [model]  resource/model-load probes from the chargen pool block (July)
 *   [vtx]    uniform/attribute layout dumps from the geometry spikes
 *   [GLSRC   shader source dump from the varying-overflow crash
 *   [res]    entry[...]  save-game key-table listing, ~70 lines per save
 * Failures still get through: the keep-rule below runs first. */
static const char *const k_log_retired[] = {
  "[model]", "[vtx]", "[GLSRC", "[res]    entry[",
  "[GL] glShaderSource(sh=%u, count=%d) ===", "[GL] glShaderSource(sh=%u) ===",
};
static const char *const k_log_keep[] = {
  "fail", "FAIL", "error", "ERROR", "Error", "WARNING", "warning",
  "MISSING", "missing", "abort", "ABORT", "CRASH", "unresolved",
};

static int log_has_tag(const char *fmt, const char *const *tags, unsigned n_tags) {
  for (unsigned i = 0; i < n_tags; i++)
    if (!strncmp(fmt, tags[i], strlen(tags[i]))) return 1;
  return 0;
}

static int log_kept(const char *fmt) {
  for (unsigned i = 0; i < sizeof k_log_keep / sizeof k_log_keep[0]; i++)
    if (strstr(fmt, k_log_keep[i])) return 1;
  return 0;
}

static int log_retired(const char *fmt) {
  if (!fmt || fmt[0] != '[') return 0;
  if (!log_has_tag(fmt, k_log_retired, sizeof k_log_retired / sizeof k_log_retired[0]))
    return 0;
  return !log_kept(fmt);
}

#if !LOG_DIAGNOSTICS
/* Release quiet. Tags are literal prefixes of the format string, so this runs
 * before any formatting: a suppressed line costs a handful of byte compares and
 * nothing else. See LOG_DIAGNOSTICS in config.h.
 *
 * The keep-rule comes first on purpose. A line that reports a failure is worth
 * more than the noise it sits in, and several of these tags carry both -- [GL]
 * emits per-window statistics and shader compile failures alike. */
static const char *const k_log_noisy[] = {
  "[GL]", "[res]", "[JNI]", "[gui]", "[FS]", "[SDL]", "[snd?]", "[heap]",
  "[big]", "[wd]", "[wd:t0]", "[wd:t1]", "[vgl]", "[input]", "[touch]",
  "[load]", "[model]",
};
static int log_suppressed(const char *fmt) {
  if (!fmt || fmt[0] != '[' || log_kept(fmt)) return 0;
  return log_has_tag(fmt, k_log_noisy, sizeof k_log_noisy / sizeof k_log_noisy[0]);
}
#endif

void log_printf(const char *fmt, ...) {
  char line[1024];

#if LOG_OFF
  if (!g_panic) return;
#endif

  if (!g_panic && log_retired(fmt)) return;
#if !LOG_DIAGNOSTICS
  if (!g_panic && log_suppressed(fmt)) return;
#endif

  // microsecond uptime stamp so ordering is unambiguous across threads
  uint64_t t = sceKernelGetProcessTimeWide();
  int n = snprintf(line, sizeof(line), "[%llu.%06llu] ",
                   (unsigned long long)(t / 1000000ULL),
                   (unsigned long long)(t % 1000000ULL));

  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
  va_end(ap);

  if (n > (int)sizeof(line) - 2)
    n = sizeof(line) - 2;
  line[n++] = '\n';
  line[n] = '\0';

#if LOG_BUF_BYTES > 0
  // Straight to the card when buffering cannot be done safely: a fault is in
  // progress, or there is no mutex yet (before log_init) / at all (creation
  // failed). Buffering without the lock would let threads interleave inside the
  // shared buffer, which is worse than the syscall it saves.
  if (g_panic || g_log_mtx < 0) {
    log_write_direct(line, n);
    return;
  }
  int held = log_lock();
  if (g_buf_n + n > LOG_BUF_BYTES) {
    // The writer is a whole buffer behind (or not running): write it out here.
    int io = io_lock();
    log_drain();
    io_unlock(io);
    g_st_sync++;
  }
  memcpy(g_bufs[g_cur] + g_buf_n, line, (size_t)n);
  g_buf_n += n;
  int kick = 0;
  if (g_writer_up) {
    if (!g_kicked && g_buf_n >= LOG_BUF_BYTES / 2) {
      g_kicked = 1;
      kick = 1;
    }
  } else if (t - g_last_flush >= (uint64_t)LOG_FLUSH_MS * 1000) {
    // No writer thread: the old inline durability floor.
    int io = io_lock();
    log_drain();
    log_close();
    io_unlock(io);
    g_last_flush = t;
  }
  log_unlock(held);
  if (kick)
    sceKernelSignalSema(g_wake, 1);
#else
  log_write_direct(line, n);
#endif
}

// ---- crash handling -------------------------------------------------------
// Hardware CPU-fault capture (PC/LR/registers) lives in crash.c via kubridge's
// kuKernelRegisterExceptionHandler. This log_fatal covers the controlled fatal
// path the loader itself takes (unresolved symbol, missing file, etc.).
void log_fatal(const char *reason) {
  log_printf("[CRASH] %s", reason ? reason : "(unknown)");
}

void log_init(void) {
  // Ensure ux0:data/kotor exists (ignore EEXIST).
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(DATA_PATH, 0777);

#if LOG_OFF
  // No log this session. Remove the last one so a log.txt left by a logged
  // build is never mistaken for this run; a crash dump recreates the file.
  sceIoRemove(LOG_PATH);
  return;
#endif

  // Truncate the log at startup.
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0)
    sceIoClose(fd);

#if LOG_BUF_BYTES > 0
  // Several threads log (game, watchdog, workers, audio). Without a lock they
  // would interleave inside the shared buffer; the kernel used to serialise
  // whole lines for us only because each line was its own write.
  g_log_mtx = sceKernelCreateMutex("kotor_log", 0, 0, NULL);
  g_io_mtx = sceKernelCreateMutex("kotor_log_io", 0, 0, NULL);
  g_last_flush = sceKernelGetProcessTimeWide();
  // Below the game and audio threads: the card can wait, they cannot.
  g_wake = sceKernelCreateSema("kotor_log_wake", 0, 0, 1, NULL);
  if (g_io_mtx >= 0 && g_wake >= 0) {
    SceUID th = sceKernelCreateThread("kotor_log", log_writer, 0x10000120, 0x4000, 0,
                                      thread_mask(CPU_AUX_B), NULL);
    if (th >= 0 && sceKernelStartThread(th, 0, NULL) >= 0) {
      g_writer_up = 1;
      thread_census_add(th, "log");
    }
  }
#endif

  log_printf("=== KOTOR Vita loader log ===");
  // Build stamp so a log is unambiguously tied to a specific .vpk (ends the
  // "did they flash the latest build?" guesswork).
  // CAVEAT: __DATE__/__TIME__ are baked when THIS file compiles, so an incremental
  // build that touches only other sources ships a stale stamp (log44 read 18:47
  // while running the 19:13 build). Always `touch loader/log.c` before building,
  // and cross-check a log against a feature line rather than this stamp alone.
  log_printf("=== BUILD " __DATE__ " " __TIME__ " ===");
#if LOG_BUF_BYTES > 0
  log_printf("=== log buffered: 2x%d KB, flush every %d ms on %s (mutex=0x%x) ===",
             LOG_BUFFER_KB, LOG_FLUSH_MS, g_writer_up ? "the writer thread" : "the logging thread",
             (unsigned)g_log_mtx);
#endif
}
