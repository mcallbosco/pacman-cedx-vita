#ifndef PACMAN_GAME_DANGER_ZOOM_H
#define PACMAN_GAME_DANGER_ZOOM_H

#include <vitaGL.h>

void game_danger_zoom_install_hooks(void);
void game_danger_zoom_viewport(GLint x, GLint y, GLsizei width, GLsizei height);
void game_danger_zoom_bind_framebuffer(GLenum target, GLuint framebuffer);
void game_danger_zoom_get_integer(GLenum name, GLint *value);

#endif
