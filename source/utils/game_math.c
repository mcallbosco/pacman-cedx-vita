#include "game_math.h"
#include "game_patch.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math")

static float (*coordinate_floor)(float);

/* Softfp arguments are raw bits. Preserve x-store before y-load when the
 * source and destination overlap, as in the native copy and assignment. */
static void *vector_from_bits(void *out, uint32_t x, uint32_t y) {
    memcpy(out, &x, 4);
    memcpy((char *)out + 4, &y, 4);
    return out;
}

static void *vector_copy(void *out, const void *in) {
    uint32_t component;
    memcpy(&component, in, 4);
    memcpy(out, &component, 4);
    memcpy(&component, (const char *)in + 4, 4);
    memcpy((char *)out + 4, &component, 4);
    return out;
}

static int32_t map_coordinate(const void *map, float value,
                              size_t offset, int32_t limit) {
    float rounded = coordinate_floor(value + 0.5f);
    float integer_bits;
    /* Keep ARM's conversion for exceptional/out-of-range input, followed by
     * the native wrapping integer addition before signed clamping. */
    __asm__("vcvt.s32.f32 %0, %1" : "=t" (integer_bits) : "t" (rounded));
    uint32_t bits, origin;
    memcpy(&bits, &integer_bits, 4);
    memcpy(&origin, (const char *)map + offset, 4);
    bits += origin;
    int32_t result;
    memcpy(&result, &bits, 4);
    if (result < 0)
        result = 0;
    if (result >= limit)
        result = limit - 1;
    return result;
}

static int32_t map_x(const void *map, float x) {
    return map_coordinate(map, x, 4, 65);
}

static int32_t map_y(const void *map, float y) {
    return map_coordinate(map, y, 8, 47);
}

static uint32_t wall_elem(const void *map, float x, float y) {
    int32_t ix = map_x(map, x);
    int32_t iy = map_y(map, y);
    return ((const uint8_t *)map)[0xc08 + 65 * iy + ix];
}

void game_math_install_hooks(void) {
    uintptr_t copy = game_patch_checked_function(
        "_ZN3sys8cVector2C2ERKS0_", 0x28, 0xc2f7ca4eu);
    uintptr_t assign = game_patch_checked_function(
        "_ZN3sys8cVector2aSERKS0_", 0x28, 0xc2f7ca4eu);
    uintptr_t construct = game_patch_checked_function(
        "_ZN3sys8cVector2C2Eff", 0x2e, 0xaaffc4b4u);
    if (copy)
        hook_addr(copy, (uintptr_t)vector_copy);
    if (assign)
        hook_addr(assign, (uintptr_t)vector_copy);
    if (construct)
        hook_addr(construct, (uintptr_t)vector_from_bits);

    uintptr_t x = game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer7GetMapXEf", 0x4c, 0xa9984bf7u);
    uintptr_t y = game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer7GetMapYEf", 0x4c, 0x270552d1u);
    uintptr_t wall = game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer11GetWallElemEff", 0x4c, 0x08d282d2u);
    if (!x || !y || !game_patch_checked_function(
            "_ZN9newPacman10cMapBuffer8zero2maxEii", 0x36, 0xcd5394dcu))
        return;
    coordinate_floor = (void *)game_patch_checked_code(
        x - 0x1cba, "map coordinate floor wrapper", 0x12, 0xc7405945u);
    if (!coordinate_floor)
        return;
    hook_addr(x, (uintptr_t)map_x);
    hook_addr(y, (uintptr_t)map_y);
    if (wall)
        hook_addr(wall, (uintptr_t)wall_elem);
}
