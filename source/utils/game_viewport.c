#include "game_viewport.h"
#include "game_patch.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static uintptr_t viewport_callback;
static void **current_renderer;
static float (*viewport_ceil)(float);

static uint32_t word(const void *p, unsigned offset) {
    uint32_t value;
    memcpy(&value, (const char *)p + offset, 4);
    return value;
}

int game_viewport_capture(GameViewport *state, uintptr_t callback) {
    state->enabled = 0;
    if (!callback)
        return 1;
    if (!viewport_callback || callback != viewport_callback)
        return 0;
    const unsigned char *renderer = *current_renderer;
    if (!renderer)
        return 1;
    const void *graphics = (void *)(uintptr_t)word(renderer, 0);
    state->identity = renderer[76] & 1;
    state->round_up = renderer[8] & 1;
    static const uint8_t offsets[9] = {12, 28, 60, 0, 16, 48, 4, 20, 52};
    for (unsigned i = 0; i < 9; ++i)
        memcpy(&state->matrix[i], renderer + 12 + offsets[i], 4);
    /* The checked getters return the last cVector2 in their raster stacks. */
    memcpy(state->scale, (void *)(uintptr_t)(word(graphics, 0x98) - 8), 8);
    memcpy(state->offset, (void *)(uintptr_t)(word(graphics, 0x8c) - 8), 8);
    state->enabled = 1;
    return 1;
}

static float project(const float *m, float x, float y) {
    float value;
    /* Native VMLA rounds its multiply before adding. Operand order also
     * determines which NaN payload survives exceptional input. */
    __asm__(
        "vmul.f32 %0, %2, %4\n"
        "vmla.f32 %0, %1, %3\n"
        "vadd.f32 %0, %0, %5\n"
        : "=&t" (value)
        : "t" (m[0]), "t" (m[1]), "t" (x), "t" (y), "t" (m[2]));
    return value;
}

void game_viewport_apply(const GameViewport *state, float *x, float *y) {
    if (!state->enabled)
        return;
    float px = *x, py = *y;
    if (!state->identity) {
        float w = project(state->matrix, px, py);
        float tx = project(state->matrix + 3, px, py);
        float ty = project(state->matrix + 6, px, py);
        __asm__("vdiv.f32 %0, %0, %2\nvdiv.f32 %1, %1, %2"
                : "+&t" (tx), "+&t" (ty) : "t" (w));
        px = tx;
        py = ty;
    }
    __asm__(
        "vmul.f32 %0, %0, %2\n"
        "vmul.f32 %1, %1, %3\n"
        "vadd.f32 %0, %0, %4\n"
        "vadd.f32 %1, %1, %5\n"
        : "+&t" (px), "+&t" (py)
        : "t" (state->scale[0]), "t" (state->scale[1]),
          "t" (state->offset[0]), "t" (state->offset[1]));
    if (state->round_up) {
        px = viewport_ceil(px);
        py = viewport_ceil(py);
    }
    *x = px;
    *y = py;
}

void game_viewport_install(void) {
    uintptr_t transform = game_patch_checked_function(
        "_ZN3sys5runny8Renderer14TransformPointERKNS_8cVector2E", 0xc4, 0x4081c677u);
    uintptr_t raster = game_patch_checked_function(
        "_ZN3sys8Graphics12GetRasterPosEv", 0x18, 0xd5d1acadu);
    uintptr_t callback = game_patch_checked_function(
        "_ZN3sys5runny8Renderer36TranformPointFromGlobalRunnyRendererERfS2_",
        0x90, 0xcf9c3747u);
    if (!transform || !raster || !callback ||
        !game_patch_checked_function("_ZN3sys8Graphics14GetRasterScaleEv", 0x18, 0x08db7dedu) ||
        !game_patch_checked_function("_ZN3sys4math6Matrix16TransformPoint2fEPfS2_", 0xd8, 0x806f0c63u) ||
        !game_patch_checked_code(raster - 0x110, "raster stack back", 0x12, 0xc9a36bc8u))
        return;
    viewport_ceil = (void *)game_patch_checked_code(
        transform + 0xc4, "viewport ceil wrapper", 0x14, 0x6acce950u);
    current_renderer = (void **)so_symbol(&so_mod,
        "_ZN3sys5runny8Renderer22g_CurrentRunnyRendererE");
    if (viewport_ceil && current_renderer)
        viewport_callback = callback;
}
