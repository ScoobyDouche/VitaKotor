/* log.c -- file logger to ux0:data/kotor/log.txt (see log.h) */

#include <vitasdk.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "log.h"

// Originally every line did its own open/append/close so a hard crash could not
// lose output. That is three memory-card syscalls per line, and once the game
// reached actual gameplay (log97: 19086 lines) it was a visible slice of the
// frame time. Keep one fd open instead and close/reopen every LOG_FLUSH_EVERY
// lines, so at most that many lines are at risk while steady-state logging costs
// a single write. There is no userland buffer either way -- sceIoWrite goes
// straight to the kernel -- and log_flush() is called explicitly on the crash
// path so fault dumps are always complete.
#define LOG_FLUSH_EVERY 64

static SceUID g_log_fd = -1;
static int g_log_since_flush = 0;

static void log_open_if_needed(void) {
  if (g_log_fd < 0)
    g_log_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
}

void log_flush(void) {
  if (g_log_fd >= 0) {
    sceIoClose(g_log_fd);
    g_log_fd = -1;
  }
  g_log_since_flush = 0;
}

static void log_write_raw(const char *buf, int len) {
  log_open_if_needed();
  if (g_log_fd < 0)
    return;
  sceIoWrite(g_log_fd, buf, len);
  if (++g_log_since_flush >= LOG_FLUSH_EVERY)
    log_flush();
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

void log_printf(const char *fmt, ...) {
  char line[1024];

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

  log_write_raw(line, n);
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

  // Truncate the log at startup.
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0)
    sceIoClose(fd);

  log_printf("=== KOTOR Vita loader log ===");
  // Build stamp so a log is unambiguously tied to a specific .vpk (ends the
  // "did they flash the latest build?" guesswork).
  // CAVEAT: __DATE__/__TIME__ are baked when THIS file compiles, so an incremental
  // build that touches only other sources ships a stale stamp (log44 read 18:47
  // while running the 19:13 build). Always `touch loader/log.c` before building,
  // and cross-check a log against a feature line rather than this stamp alone.
  log_printf("=== BUILD " __DATE__ " " __TIME__ " ===");
}
