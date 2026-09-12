#ifndef PMCEDX_GAME_RESOLUTION_H
#define PMCEDX_GAME_RESOLUTION_H

#include <vitaGL.h>

int game_resolution_requested(void);
void game_resolution_enable(void);
void game_resolution_begin(void);
void game_resolution_present(void);
void game_resolution_viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void game_resolution_scissor(GLint x, GLint y, GLsizei width, GLsizei height);
void game_resolution_bind_framebuffer(GLenum target, GLuint framebuffer);
void game_resolution_get_integer(GLenum name, GLint *value);

#endif
