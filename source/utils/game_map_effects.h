#ifndef PMCEDX_GAME_MAP_EFFECTS_H
#define PMCEDX_GAME_MAP_EFFECTS_H

#include <vitaGL.h>

void game_map_effects_install_hooks(void);
void game_map_effects_next_frame(void);
void game_map_effects_apply(GLuint program);
void game_map_effects_register(GLuint program);
void game_map_effects_invalidate(GLuint program);
int game_map_effects_uniform_count(GLuint program);
void game_map_effects_power_pellet(void);
char *game_map_effects_shader(const char *source);
char *game_map_effects_single_shader(const char *source);

#endif
