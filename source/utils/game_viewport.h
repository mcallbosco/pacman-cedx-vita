#ifndef PMCEDX_GAME_VIEWPORT_H
#define PMCEDX_GAME_VIEWPORT_H

#include <stdint.h>

typedef struct {
    uint32_t enabled, identity, round_up;
    float matrix[9], scale[2], offset[2];
} GameViewport;

void game_viewport_install(void);
/* A snapshot is valid only until renderer/raster state can change. Unknown
 * callbacks return zero so the caller can retain its native drawing path. */
int game_viewport_capture(GameViewport *state, uintptr_t callback);
void game_viewport_apply(const GameViewport *state, float *x, float *y);

#endif
