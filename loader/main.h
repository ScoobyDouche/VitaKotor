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

int ret0(void);
int ret1(void);

extern SceTouchPanelInfo panelInfoFront, panelInfoBack;

#endif
