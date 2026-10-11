/* script_fix.h -- byte patches for bugs in the game's own compiled scripts.
 *
 * Every .ncs the game loads passes through CResNCS::OnResourceServiced. We
 * swap that vtable slot for a wrapper that runs the original, then checks the
 * loaded bytes against a short list of known-broken scripts. A script is only
 * touched when its size and CRC match the shipped original exactly, so a mod
 * that replaces it, or a second load of an already patched buffer, is left
 * alone. No game data ships with the port; the fix is applied in memory.
 *
 * Griff never spawning in the Sand People enclave (tat_m20aa):
 *   k_ptat20aa_enter spawns Griff when Mis_MissionTalk == 9 and area plot
 *   flag 0 is clear, then sets flag 0 so it only happens once. Opening one of
 *   the Sand People's chests runs k_ptat_trysteal ("they are watching very
 *   closely"), which ALSO sets area flag 0 to remember the warning, and
 *   k_ptat_sandsteal reads it back. Open a chest before Mission's quest
 *   reaches the Czerka step and Griff is treated as already spawned, forever.
 *   Both theft scripts load the flag index from global SW_PLOT_BOOLEAN_01
 *   (CPTOPBP -612); pointing them at SW_PLOT_BOOLEAN_10 (CPTOPBP -576), which
 *   no enclave script uses on the area, separates the two.
 *
 *   Saves where a chest was already opened still have flag 0 set; the fix
 *   prevents the collision, it does not repair a save that already hit it. */
#pragma once

#include <stddef.h>

/* Patch `buf` in place if it is one of the known-broken scripts. Returns the
 * script's name when a patch was applied, NULL otherwise. Pure: no game or
 * Vita dependencies, so it can be tested on the host. */
const char *script_fix_apply(unsigned char *buf, size_t len);

/* Hook CResNCS::OnResourceServiced. Call after libKOTOR is relocated. */
void script_fix_install(void);
