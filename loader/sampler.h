/* sampler.h -- a statistical profile of the game thread, by call site.
 *
 * The Vita gives userland no way to read another thread's PC, so a classic
 * sampler is out. But libKOTOR and libandroid_port are built -fPIC with
 * default visibility: every call between functions, even to a three-line
 * vector helper, goes through a PLT slot. Each slot is pointed at a stub that
 * tests one flag; a timer thread raises the flag every millisecond, and the
 * next call the game thread makes records its return address and clears it.
 *
 * So a sample says "one millisecond tick landed while the game thread was in
 * the function containing this call site" -- or in a leaf that made no calls
 * and has since returned to it. Read the top call sites against the
 * disassembly. Off unless swkotor.ini [Vita Options] Profile=1; it costs a few
 * instructions per call and turns the AI overlap off for the run.
 *
 * Profile=2 is a different tool on the same trick: it counts every call
 * libKOTOR makes into the port layer, by function (see census_install).
 */
#pragma once

#include <vitasdk.h>

/* Point every PLT slot of both game libraries at a sampling stub and start
 * the tick thread. `game_thid` is read at each sample, so it may still be
 * unset now. Call after the ini is read and before srv_overlap_install. */
void sampler_install(volatile SceUID *game_thid, int mode);
