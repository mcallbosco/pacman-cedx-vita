#include "game_map_draw.h"
#include "game_patch.h"
#include "game_batch.h"
#include "game_viewport.h"
#include "glutil.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static uintptr_t map_loop_resume __attribute__((used));
static uintptr_t map_loop_done __attribute__((used));
static void **map_graphics;
static void *(*map_profile)(void);
static int (*map_shaders)(void *);
static int (*map_batches)(void *);

static uint32_t word(const void *p, unsigned offset) {
    uint32_t value;
    memcpy(&value, (const char *)p + offset, 4);
    return value;
}

static float number(const void *p, unsigned offset) {
    float value;
    memcpy(&value, (const char *)p + offset, 4);
    return value;
}

/* The map branch passes unscaled=true to ConvPos, then AddBatch converts
 * coordinates to signed integers and back to floats. Keep both roundings. */
static void position(float *out, const void *point, float cx, float cy,
                     float px, float py, float sine, float cosine,
                     const GameViewport *viewport, int callback) {
    float x = (number(point, 4) + number(point, 28)) - cx;
    float y = (number(point, 8) + number(point, 32)) - cy;
    float rx, ry;
    __asm__(
        "vneg.f32 %0, %2\n"
        "vmul.f32 %0, %0, %5\n"
        "vmla.f32 %0, %3, %4\n"
        "vmul.f32 %1, %3, %5\n"
        "vmla.f32 %1, %2, %4\n"
        : "=&t" (rx), "=&t" (ry)
        : "t" (sine), "t" (cosine), "t" (x), "t" (y));
    rx += px;
    ry += py;
    __asm__("vcvt.s32.f32 %0, %0\nvcvt.f32.s32 %0, %0" : "+t" (rx));
    __asm__("vcvt.s32.f32 %0, %0\nvcvt.f32.s32 %0, %0" : "+t" (ry));
    if (callback) {
        game_viewport_apply(viewport, &rx, &ry);
        __asm__("vcvt.s32.f32 %0, %0\nvcvt.f32.s32 %0, %0" : "+t" (rx));
        __asm__("vcvt.s32.f32 %0, %0\nvcvt.f32.s32 %0, %0" : "+t" (ry));
    }
    out[0] = rx;
    out[1] = ry;
}

static int __attribute__((used, noinline)) emit_map_strip(const void *frame) {
    const unsigned char *sprite = (void *)word(frame, 0x54c);
    unsigned width = word(sprite, 0x20);
    if (sprite[0x98] != 1 || sprite[0xa0] != 4 ||
        !width || width > 64 || word(sprite, 0x24) != 1)
        return 0;
    void *graphics = *map_graphics;
    unsigned stride = word(sprite, 0x50) ? 40 : 32;
    if (!game_batch_direct_grid_supported() || !graphics || !gl_batch_can_index(stride))
        return 0;
    uintptr_t callback = word(graphics, 0x14);
    void *profile = map_profile();
    if (!map_shaders(profile) || !map_batches(profile))
        return 0;
    unsigned count = word(frame, 0x5a0);
    float *buffer = (void *)word(graphics, 0x3c);
    const unsigned char *points = (void *)word(sprite, 0x1c);
    if (!buffer || !points || !count || count > width || word(graphics, 0x40))
        return 0;
    GameViewport viewport;
    if (!game_viewport_capture(&viewport, callback))
        return 0;

    /* Native setup has already selected the effect, palette and blend mode,
     * allocated the batch and calculated these exact UV/trig values. */
    float du = number(frame, 0x58c), dv = number(frame, 0x588);
    float sine = number(frame, 0xcd8), cosine = number(frame, 0xcd4);
    float cx = number(sprite, 0x44) * number(sprite, 0x5c);
    float cy = number(sprite, 0x48) * number(sprite, 0x60);
    float xy[130][2];
    for (unsigned i = 0; i < 2 * (width + 1); ++i)
        position(xy[i], points + i * 52, cx, cy,
                 number(sprite, 0x54), number(sprite, 0x58), sine, cosine, &viewport, callback != 0);

    float *cursor = buffer;
    for (unsigned x = 0; x < width; ++x) {
        const unsigned char *cell = points + x * 52;
        if (!word(cell, 0))
            continue;
        float u[2] = {number(cell, 12), 0}, v[2] = {number(cell, 16), 0};
        float su[2] = {number(cell, 20), 0}, sv[2] = {number(cell, 24), 0};
        u[1] = sprite[0x3c] & 1 ? u[0] - du : u[0] + du;
        v[1] = sprite[0x3c] & 2 ? v[0] - dv : v[0] + dv;
        su[1] = sprite[0x40] & 1 ? su[0] - du : su[0] + du;
        sv[1] = sprite[0x40] & 2 ? sv[0] - dv : sv[0] + dv;
        /* Four vertices in the existing GL_QUADS order: TL, TR, BR, BL.
         * UVs belong to the cell; positions and colours belong to its corners. */
        for (unsigned i = 0; i < 4; ++i) {
            unsigned right = i == 1 || i == 2, bottom = i >= 2;
            unsigned point = x + right + bottom * (width + 1);
            memcpy(cursor, xy[point], 8);
            cursor[2] = u[right];
            cursor[3] = v[bottom];
            if (stride == 40) {
                cursor[4] = su[right];
                cursor[5] = sv[bottom];
            }
            memcpy(cursor + stride / 4 - 4, points + point * 52 + 36, 16);
            cursor += stride / 4;
        }
    }
    ((unsigned char *)graphics)[0x85] = stride == 40;
    memcpy((char *)graphics + 0x3c, &cursor, 4);
    count *= 4;
    memcpy((char *)graphics + 0x40, &count, 4);
    game_batch_direct_grid(graphics, buffer, count, stride);
    return 1;
}

static void __attribute__((naked)) map_loop_bridge(void) {
    __asm__ volatile(
        "mov r0, sp\n"
        "push {r4, lr}\n"
        "bl emit_map_strip\n"
        "cmp r0, #0\n"
        "pop {r4, lr}\n"
        "bne 1f\n"
        "movs r0, #0\n"
        "str r0, [sp, #1400]\n"
        "ldr r12, =map_loop_resume\n"
        "ldr r12, [r12]\n"
        "bx r12\n"
        "1: ldr r12, =map_loop_done\n"
        "ldr r12, [r12]\n"
        "bx r12\n"
        ".ltorg\n");
}

void game_map_draw_install(uintptr_t grid) {
    map_graphics = (void *)so_symbol(&so_mod, "_ZN3sys11g_pGraphicsE");
    map_profile = (void *)game_patch_checked_function(
        "_ZN3sys9SingletonINS_13DeviceProfileEE11GetInstanceEv", 0x48, 0x857a53beu);
    map_shaders = (void *)so_symbol(&so_mod, "_ZN3sys13DeviceProfile10UseShadersEv");
    map_batches = (void *)so_symbol(&so_mod, "_ZN3sys13DeviceProfile22UseBatchesOptimizationEv");
    if (!map_graphics || !map_profile || !map_shaders || !map_batches)
        return;
    map_loop_resume = grid + 0x1bcc;
    map_loop_done = grid + 0x4e38;
    hook_addr(grid + 0x1bc4, (uintptr_t)map_loop_bridge);
}
