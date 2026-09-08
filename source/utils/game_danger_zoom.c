#include "game_danger_zoom.h"
#include "game_patch.h"
#include "settings.h"
#include "game_frame_rate.h"
#include "game_resolution.h"
#include "game_batch.h"

#include <math.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static uintptr_t draw_resume __attribute__((used));
static void (*sprite_draw_reset)(void) __attribute__((used));
static int (*is_game)(void), (*is_preview)(void), (*is_paused)(void);
static int (*slow_type)(void);
static float (*slow_rate)(void);
static void (*end_batches)(void *);
static void **graphics;
static const unsigned char *pacman;
static float amount, focus_x, focus_y;
static int drawing;
static GLint viewport[4], scene_target, bound_target;
static int split_ui;
static uintptr_t overlay_resume __attribute__((used));
static void *(*primitive_list_get)(int) __attribute__((used));

static float number(const void *object, unsigned offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static float unit(float value) {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static int update_zoom(void) {
    if (!setting_dangerZoom || !is_game() || is_preview() || is_paused() ||
        !(pacman[30] & 1) || (pacman[33] & 1)) {
        amount = 0.0f;
        return 0;
    }
    float x = number(pacman, 0), y = number(pacman, 4);
    if (!isfinite(x) || !isfinite(y)) {
        amount = 0.0f;
        return 0;
    }
    float target = 0.0f;
    /* Type zero is automatic danger slowdown; manual slow and recovery to
     * normal play must not start a new zoom. Keep native timing untouched. */
    if (slow_type() == 0) {
        float rate = slow_rate();
        if (isfinite(rate))
            target = 0.08f * unit((1.0f - rate) / 0.8f);
    }
    for (int i = 0; i < game_frame_rate_render_ticks(); ++i)
        amount += (target - amount) * (target > amount ? 0.2f : 0.12f);
    if (amount < 0.0001f) {
        amount = 0.0f;
        return 0;
    }
    /* The native screen-effect record uses the 1280x720 game canvas.
     * Normalize before applying it to the current physical viewport. */
    focus_x = unit(x / 1280.0f);
    focus_y = 1.0f - unit(y / 720.0f);
    return 1;
}

static void apply_viewport(void) {
    GLint x = viewport[0], y = viewport[1];
    GLsizei w = viewport[2], h = viewport[3];
    if (bound_target == scene_target && w > 0 && h > 0 && w <= 4096 && h <= 4096 &&
        x >= -16384 && x <= 16384 && y >= -16384 && y <= 16384) {
        /* Keep enlarged sizes even for consistent zoom rounding. */
        GLsizei zw = (GLsizei)(w * (1.0f + amount) * 0.5f + 0.5f) * 2;
        GLsizei zh = (GLsizei)(h * (1.0f + amount) * 0.5f + 0.5f) * 2;
        x -= (GLint)((zw - w) * focus_x + 0.5f);
        y -= (GLint)((zh - h) * focus_y + 0.5f);
        w = zw;
        h = zh;
    }
    game_resolution_viewport(x, y, w, h);
}

void game_danger_zoom_viewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!drawing || width < 0 || height < 0) {
        game_resolution_viewport(x, y, width, height);
        return;
    }
    viewport[0] = x; viewport[1] = y;
    viewport[2] = width; viewport[3] = height;
    apply_viewport();
}

void game_danger_zoom_bind_framebuffer(GLenum target, GLuint framebuffer) {
    game_resolution_bind_framebuffer(target, framebuffer);
    if (drawing) {
        game_resolution_get_integer(GL_FRAMEBUFFER_BINDING, &bound_target);
        apply_viewport();
    }
}

void game_danger_zoom_get_integer(GLenum name, GLint *value) {
    /* Native save/restore sequences see their requested viewport, never an
     * already enlarged one. Nested texture targets retain normal dimensions. */
    if (drawing && name == GL_VIEWPORT)
        memcpy(value, viewport, sizeof(viewport));
    else
        game_resolution_get_integer(name, value);
}

/* Replay the checked prologue and displaced call. The native epilogue
 * unwinds this frame, so the draw entry stays patched throughout play. */
static void __attribute__((naked, noinline)) draw_original(void) {
    __asm__(
        "push {r7, lr}\n"
        "mov r7, sp\n"
        "sub sp, #8\n"
        "ldr ip, =sprite_draw_reset\n"
        "ldr ip, [ip]\n"
        "blx ip\n"
        "ldr ip, =draw_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

static void __attribute__((used, noinline)) finish_world(void) {
    if (*graphics)
        end_batches(*graphics);
    if (drawing) {
        drawing = 0;
        game_resolution_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
    game_resolution_present();
}

/* List 4 contains the native HUD, prompts and fades. Preserve its ordering,
 * projection and draw callbacks, after resolving the lower-resolution world. */
static void __attribute__((naked, noinline)) overlay_bridge(void) {
    __asm__(
        "push {r4, lr}\n"
        "bl finish_world\n"
        "movs r0, #4\n"
        "ldr ip, =primitive_list_get\n"
        "ldr ip, [ip]\n"
        "blx ip\n"
        "pop {r4, lr}\n"
        "str r0, [sp, #4]\n"
        "ldr ip, =overlay_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

static void draw_zoom(void) {
    int enabled = update_zoom() && *graphics;
    if ((split_ui || enabled) && *graphics)
        end_batches(*graphics);
    if (split_ui && *graphics)
        game_resolution_begin();
    if (enabled) {
        game_resolution_get_integer(GL_VIEWPORT, viewport);
        game_resolution_get_integer(GL_FRAMEBUFFER_BINDING, &scene_target);
        bound_target = scene_target;
        drawing = 1;
        apply_viewport();
    }
    draw_original();
    if (drawing) {
        /* Submit the final game batch before returning to the UI viewport. */
        finish_world();
    }
}

void game_danger_zoom_install_hooks(void) {
    if ((!setting_dangerZoom && !game_resolution_requested()) || !game_batch_flush_available())
        return;
    uintptr_t draw = game_patch_checked_function(
        "_ZN9newPacman8LoopDrawEv", 0xa0, 0x79998f8du);
    sprite_draw_reset = (void *)game_patch_checked_function(
        "_ZN3sys7cSprite15SpriteDrawResetEv", 0x2c, 0xa5e78769u);
    is_game = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence6IsGameEv", 0x14, 0x476f8a74u);
    is_preview = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence9IsPreviewEv", 0x14, 0x1128e4ffu);
    is_paused = (void *)game_patch_checked_function(
        "_ZN9newPacman12GetPauseFlagEv", 0x10, 0xd3471ef9u);
    slow_type = (void *)game_patch_checked_function(
        "_ZN9newPacman10cSlowSpeed7GetTypeEv", 0x10, 0xa5497a10u);
    slow_rate = (void *)game_patch_checked_function(
        "_ZN9newPacman10cSlowSpeed11GetSlowRateEv", 0x5c, 0xfd4c29c1u);
    /* Base Graphics::EndBatches is empty. Use the checked concrete renderer
     * Flush captured before its batching patches, so queued geometry cannot
     * cross framebuffer or viewport transitions. */
    end_batches = game_batch_flush;
    graphics = (void *)so_symbol(&so_mod, "_ZN3sys11g_pGraphicsE");
    pacman = (void *)so_symbol(&so_mod, "_ZN9newPacman17CStaticEffectInfo6pacmanE");
    if (!draw || !sprite_draw_reset || !is_game || !is_preview || !is_paused || !slow_type || !slow_rate ||
        !end_batches || !graphics || !pacman || !game_patch_checked_function(
            "_ZN9newPacman10cSlowSpeed4FuncEv", 0x384, 0xc53fd9a1u) ||
        !game_patch_checked_function(
            "_ZN9newPacman13cOnPacmanTask4FuncEv", 0xef0, 0x00ae5623u))
        return;
    if (game_resolution_requested()) {
        primitive_list_get = (void *)game_patch_checked_function(
            "_ZN3sys16PrimitiveListGetENS_14PrimitiveValueE", 0x2c, 0x184ec3e1u);
        split_ui = primitive_list_get && game_patch_checked_function(
            "_ZN9newPacman19cOnSystemLetterTask4DrawEv", 0xf8, 0x1341c1cfu) &&
            game_patch_checked_function("_ZN9newPacman17cOnGuiCharEmuTask4DrawEv", 0x40, 0x3c1518e6u);
        if (split_ui) {
            game_resolution_enable();
            overlay_resume = draw + 0x78;
            hook_addr(draw + 0x70, (uintptr_t)overlay_bridge);
        }
    }
    if (!setting_dangerZoom && !split_ui)
        return;
    draw_resume = draw + 0x0a;
    hook_addr(draw, (uintptr_t)draw_zoom);
}
