#include "game_resolution.h"
#include "settings.h"
#include <stdint.h>
#include <string.h>

static int enabled, active, allocation_failed;
static GLuint scene_texture, scene_framebuffer;
static int width, height;
static int scale_quarters;
static GLint display_viewport[4], scene_viewport[4], scene_scissor[4];
static GLint previous_read;

int game_resolution_requested(void) {
    return setting_nativeUi && settings_resolution_width(setting_resolution) < 960;
}

void game_resolution_enable(void) {
    enabled = game_resolution_requested();
    width = settings_resolution_width(setting_resolution);
    height = settings_resolution_height(setting_resolution);
    /* Both reduced presets use exact quarter steps of the native dimensions. */
    scale_quarters = width / 240;
}

static GLint scale_nearest(GLint value) {
    int64_t numerator = (int64_t)value * scale_quarters;
    return (GLint)((numerator + (numerator >= 0 ? 2 : -2)) / 4);
}

static int64_t scale_floor(int64_t value) {
    int64_t numerator = value * scale_quarters;
    return (numerator - (numerator < 0 ? 3 : 0)) / 4;
}

static int64_t scale_ceil(int64_t value) {
    int64_t numerator = value * scale_quarters;
    return (numerator + (numerator > 0 ? 3 : 0)) / 4;
}

static int world_bound(void) {
    if (!active)
        return 0;
    GLint target;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    return (GLuint)target == scene_framebuffer;
}

void game_resolution_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (active && w >= 0 && h >= 0) {
        const GLint requested[] = {x, y, w, h};
        memcpy(scene_viewport, requested, sizeof(scene_viewport));
    }
    if (w >= 0 && h >= 0 && world_bound()) {
        x = scale_nearest(x);
        y = scale_nearest(y);
        w = scale_nearest(w);
        h = scale_nearest(h);
    }
    glViewport(x, y, w, h);
}

void game_resolution_scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (active && w >= 0 && h >= 0) {
        const GLint requested[] = {x, y, w, h};
        memcpy(scene_scissor, requested, sizeof(scene_scissor));
    }
    if (w >= 0 && h >= 0 && world_bound()) {
        GLint left = (GLint)scale_floor(x), bottom = (GLint)scale_floor(y);
        /* Widen before adding offscreen bounds, and preserve empty clips. */
        w = w ? (GLsizei)(scale_ceil((int64_t)x + w) - left) : 0;
        h = h ? (GLsizei)(scale_ceil((int64_t)y + h) - bottom) : 0;
        x = left;
        y = bottom;
    }
    glScissor(x, y, w, h);
}

void game_resolution_bind_framebuffer(GLenum target, GLuint framebuffer) {
    /* The Android renderer can nest its own texture targets and restore zero.
     * Only its default framebuffer is redirected during the world pass. */
    glBindFramebuffer(target, active && !framebuffer ? scene_framebuffer : framebuffer);
    if (active && (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)) {
        /* Viewport/scissor are global GL state, including across nested FBOs. */
        game_resolution_viewport(scene_viewport[0], scene_viewport[1], scene_viewport[2], scene_viewport[3]);
        game_resolution_scissor(scene_scissor[0], scene_scissor[1], scene_scissor[2], scene_scissor[3]);
    }
}

void game_resolution_get_integer(GLenum name, GLint *value) {
    if (active && (name == GL_VIEWPORT || name == GL_SCISSOR_BOX)) {
        memcpy(value, name == GL_VIEWPORT ? scene_viewport : scene_scissor, sizeof(scene_viewport));
        return;
    }
    glGetIntegerv(name, value);
    if (active && (name == GL_FRAMEBUFFER_BINDING || name == GL_READ_FRAMEBUFFER_BINDING) &&
        (GLuint)*value == scene_framebuffer)
        *value = 0;
}

static int create_target(void) {
    if (scene_framebuffer)
        return 1;
    if (allocation_failed)
        return 0;
    allocation_failed = 1;
    GLint texture, read, draw;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &draw);
    glGenTextures(1, &scene_texture);
    if (scene_texture) {
        glBindTexture(GL_TEXTURE_2D, scene_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        if (vglGetTexDataPointer(GL_TEXTURE_2D)) {
            glGenFramebuffers(1, &scene_framebuffer);
            if (scene_framebuffer) {
                glBindFramebuffer(GL_FRAMEBUFFER, scene_framebuffer);
                /* Attach once: reattaching each frame discards vitaGL's cached
                 * depth surface and causes repeated GPU allocations. */
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene_texture, 0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    glDeleteFramebuffers(1, &scene_framebuffer);
                    scene_framebuffer = 0;
                }
            }
        }
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindTexture(GL_TEXTURE_2D, texture);
    if (!scene_framebuffer && scene_texture) {
        glDeleteTextures(1, &scene_texture);
        scene_texture = 0;
    }
    /* On failure, leave the native display path available for this launch. */
    return scene_framebuffer != 0;
}

void game_resolution_begin(void) {
    if (!enabled || active)
        return;
    GLint target;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    if (target || !create_target())
        return;
    glGetIntegerv(GL_VIEWPORT, display_viewport);
    memcpy(scene_viewport, display_viewport, sizeof(scene_viewport));
    glGetIntegerv(GL_SCISSOR_BOX, scene_scissor);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    GLfloat clear[4];
    GLboolean mask[4], clipped = glIsEnabled(GL_SCISSOR_TEST);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_framebuffer);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    active = 1;
    game_resolution_viewport(display_viewport[0], display_viewport[1], display_viewport[2], display_viewport[3]);
    game_resolution_scissor(scene_scissor[0], scene_scissor[1], scene_scissor[2], scene_scissor[3]);
    if (clipped)
        glEnable(GL_SCISSOR_TEST);
}

void game_resolution_present(void) {
    if (!active)
        return;
    active = 0;
    GLboolean clipped = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* This vitaGL build flips FBO raster Y; its blit does not flip UVs. */
    glBlitNamedFramebuffer(scene_framebuffer, 0, 0, height, width, 0,
                          0, 0, 960, 544, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
    glViewport(display_viewport[0], display_viewport[1], display_viewport[2], display_viewport[3]);
    glScissor(scene_scissor[0], scene_scissor[1], scene_scissor[2], scene_scissor[3]);
    if (clipped)
        glEnable(GL_SCISSOR_TEST);
}
