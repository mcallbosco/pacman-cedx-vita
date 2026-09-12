#ifndef PMCEDX_GAME_FRAME_RATE_H
#define PMCEDX_GAME_FRAME_RATE_H

#include <stdbool.h>

void game_frame_rate_install_hooks(void);
void game_frame_rate_begin_tick(void);
bool game_frame_rate_should_render(void);
int game_frame_rate_render_ticks(void);

#endif
