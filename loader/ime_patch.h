/* ime_patch.h -- Vita on-screen keyboard for the game's text fields
 *
 * The chargen name box, the save-game name box and every other CSWGuiEditbox
 * are typed into through the platform's virtual keyboard. On Android that is
 * the system IME; on Vita it is sceImeDialog, wired up in ime_patch.c.
 */

#ifndef __IME_PATCH_H__
#define __IME_PATCH_H__

#include "so_util.h"

// Resolver entries this layer contributes: the two ASLPlat_* keyboard entry
// points, overriding libandroid_port's Android-only implementations.
const so_default_dynlib *ime_get_dynlib(void);
extern const int ime_dynlib_size;

// Non-zero while the IME dialog owns the screen. The swap hook uses this to
// composite the common dialog into the frame, and the touch pump uses it to
// stop feeding the game finger events aimed at the keyboard.
int ime_dialog_active(void);

// Poll the dialog and, once the user confirms, queue what they typed as
// SDL_TEXTINPUT events. Call once per frame from the swap hook (game thread).
void ime_pump(void);

#endif
