#include "game_sprite_draw.h"
#include "game_patch.h"
#include "game_batch.h"
#include "game_transform.h"
#include "game_viewport.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static void **sprite_graphics;
static uintptr_t sprite_draw_resume __attribute__((used));
static uintptr_t sprite_draw_done __attribute__((used));

static uint32_t word(const void *p, unsigned offset) {
    uint32_t value;
    memcpy(&value, (const char *)p + offset, 4);
    return value;
}

static int __attribute__((used, noinline)) emit_sprite(const void *frame) {
    if (!game_batch_direct_grid_supported())
        return 0;
    void *graphics = *sprite_graphics;
    if (!graphics)
        return 0;
    GameViewport viewport;
    if (!game_viewport_capture(&viewport, word(graphics, 0x14)))
        return 0;
    const void *sprite = (void *)word(frame, 0x230);
    float xy[4][2];
    if (!game_transform_quad(xy, sprite))
        return 0;
    uint32_t vertices[4][8];
    unsigned flip_x = ((const unsigned char *)frame)[0x243] & 1;
    unsigned flip_y = ((const unsigned char *)frame)[0x242] & 1;
    for (unsigned i = 0; i < 4; ++i) {
        /* Native DrawNormal truncates before AddBatch's viewport transform,
         * and TransformPointi truncates again even with no callback. */
        float x = xy[i][0], y = xy[i][1];
        __asm__("vcvt.s32.f32 %0, %0\nvcvt.f32.s32 %0, %0" : "+t" (x));
        __asm__("vcvt.s32.f32 %0, %0\nvcvt.f32.s32 %0, %0" : "+t" (y));
        game_viewport_apply(&viewport, &x, &y);
        __asm__("vcvt.s32.f32 %0, %0\nvcvt.f32.s32 %0, %0" : "+t" (x));
        __asm__("vcvt.s32.f32 %0, %0\nvcvt.f32.s32 %0, %0" : "+t" (y));
        memcpy(vertices[i], &x, 4);
        memcpy(vertices[i] + 1, &y, 4);
        vertices[i][2] = word(frame, ((i & 1) != flip_x) ? 0x45c : 0x464);
        vertices[i][3] = word(frame, (((i >> 1) & 1) != flip_y) ? 0x460 : 0x468);
        memcpy(vertices[i] + 4, (const char *)sprite + 0x78, 16);
    }
    return game_batch_append_sprite(graphics, vertices);
}

static void __attribute__((naked)) sprite_draw_bridge(void) {
    __asm__ volatile(
        "mov r0, sp\n"
        "push {r4, lr}\n"
        "bl emit_sprite\n"
        "cmp r0, #0\n"
        "pop {r4, lr}\n"
        "bne 1f\n"
        "add r0, sp, #936\n"
        "movs r1, #0\n"
        "str r0, [sp, #412]\n"
        "str r1, [sp, #408]\n"
        "ldr r2, [sp, #408]\n"
        "ldr r12, =sprite_draw_resume\n"
        "ldr r12, [r12]\n"
        "bx r12\n"
        "1: ldr r12, =sprite_draw_done\n"
        "ldr r12, [r12]\n"
        "bx r12\n"
        ".ltorg\n");
}

void game_sprite_draw_install(uintptr_t draw) {
    sprite_graphics = (void *)so_symbol(&so_mod, "_ZN3sys11g_pGraphicsE");
    if (!sprite_graphics)
        return;
    /* The caller checked the complete original DrawNormal before its existing
     * corner patches. Preserve native shader/UV/batch setup and EndBatches. */
    sprite_draw_resume = draw + 0x690;
    sprite_draw_done = draw + 0xc7e;
    hook_addr(draw + 0x686, (uintptr_t)sprite_draw_bridge);
}
