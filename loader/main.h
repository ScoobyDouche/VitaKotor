/* main.h -- shared loader declarations */

#ifndef __MAIN_H__
#define __MAIN_H__

#include <psp2/touch.h>
#include "so_util.h"

extern so_module kotor_mod;   // libKOTOR.so
extern so_module port_mod;    // libandroid_port.so
extern so_module lzma_mod;    // libLzmaLib.so (LzmaUncompress, used by hints.c)

int debugPrintf(const char *text, ...);
void fatal_error(const char *fmt, ...);

/* The language the loader settled on, as an INI_LANG_* id: what swkotor.ini
 * said, or what the picker chose. Read by the boot screen, which has its own
 * text to put on the screen in that language. */
int loader_language(void);

int ret0(void);
int ret1(void);

extern SceTouchPanelInfo panelInfoFront, panelInfoBack;

#endif
