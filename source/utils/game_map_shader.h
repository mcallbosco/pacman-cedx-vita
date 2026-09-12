#ifndef PMCEDX_GAME_MAP_SHADER_H
#define PMCEDX_GAME_MAP_SHADER_H

#include <vitaGL.h>
void game_map_shader_sources(GLuint program, const char *vertex, const char *fragment);
void game_map_shader_invalidate(GLuint program);
void game_map_shader_prepare(void);
int game_map_shader_draw(GLuint program, GLfloat light, const void *vertices,
                         unsigned count);

#endif
