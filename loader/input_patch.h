/* input_patch.h -- Vita front-touch -> SDL finger events (see input_patch.c) */
#ifndef INPUT_PATCH_H
#define INPUT_PATCH_H

// Enable front-panel touch sampling. Call once, after SDL video is up.
void input_touch_init(void);

// Sample the front touch panel and push SDL_FINGERDOWN/MOTION/UP events for any
// state change. Call once per frame (from the swap hook, on the game thread).
void input_touch_pump(void);

#endif
