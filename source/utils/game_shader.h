#ifndef PMCEDX_GAME_SHADER_H
#define PMCEDX_GAME_SHADER_H

#include <stdint.h>

void game_shader_install_hooks(void);
void game_shader_invalidate_program(uint32_t program);
int game_shader_screen_size(float *width, float *height);

#endif
