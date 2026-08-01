#include "game_grid.h"
#include "game_patch.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math")

static so_hook grid_color_hook;
static void **game_model_instance;
static const uint8_t *darker_map;

static void grid_change_color(void *sprite, const void *color) {
    uint8_t *point;
    uint32_t width, height;
    memcpy(&point, (char *)sprite + 0x1c, sizeof(point));
    memcpy(&width, (char *)sprite + 0x20, sizeof(width));
    memcpy(&height, (char *)sprite + 0x24, sizeof(height));
    if (!point || !width || !height)
        return;

    uint64_t count = ((uint64_t)width + 1) * ((uint64_t)height + 1);
    uintptr_t delta = (uintptr_t)color - (uintptr_t)point;
    if (!*game_model_instance || count > UINT32_MAX / 52 ||
        delta < count * 52) {
        so_hook_unpatch(&grid_color_hook);
        ((void (*)(void *, const void *))grid_color_hook.thumb_addr)(sprite, color);
        so_hook_repatch(&grid_color_hook);
        return;
    }

    float value;
    memcpy(&value, color, sizeof(value));
    static const uint32_t colors[2][2][3] = {
        {{0x3f800000, 0x3f800000, 0x3f800000},
         {0x3f4ccccd, 0x3ee66666, 0x3e800000}},
        {{0x3f4ccccd, 0x3f400000, 0x3f0ccccd},
         {0x3f333333, 0x3eb33333, 0x3e19999a}}
    };
    const uint32_t *rgb = colors[*darker_map & 1][value != 0.0f];
    for (uint32_t i = 0; i < (uint32_t)count; ++i, point += 52)
        memcpy(point + 36, rgb, 3 * sizeof(*rgb));
}

static void install_grid_color(void) {
    uintptr_t color = game_patch_checked_function(
        "_ZN3sys7cSprite19GridVertChangeColorENS_8cVector4E", 0x138, 0x65f73451u);
    uintptr_t model_getter = game_patch_checked_function(
        "_ZN3sys9SingletonIN6pmcedx9GameModelEE11GetInstanceEv", 0x48, 0x080407f8u);
    game_model_instance = (void **)so_symbol(&so_mod,
        "_ZN3sys9SingletonIN6pmcedx9GameModelEE11s_pInstanceE");
    darker_map = (const uint8_t *)so_symbol(&so_mod,
        "_ZN6pmcedx9GameModel15m_bDarkerMapHSVE");
    if (color && model_getter && game_model_instance && darker_map)
        grid_color_hook = hook_addr(color, (uintptr_t)grid_change_color);
}

void game_grid_install_hooks(void) {
    install_grid_color();
    uintptr_t grid = game_patch_checked_function(
        "_ZN3sys7cSprite8DrawGridEv", 0x4ec4, 0x269aca4cu);
    if (!grid || !game_patch_checked_function(
            "_ZN3sys7cSprite7ConvPosENS_8cVector2Eb", 0x18c, 0xd7692dceu))
        return;

    static const uint16_t offsets[] = {
        0x404, 0x4b4, 0x55e, 0x5f8, 0x6a0, 0x748,
        0x7f8, 0x8ca, 0x9ac, 0xa7e, 0xb5e, 0xc48,
        0x1d1a, 0x1de6, 0x1ed0, 0x1fac, 0x2094, 0x2188,
        0x243a, 0x24f2, 0x260a, 0x26c2, 0x27fa, 0x28b6,
        0x29d2, 0x2a8e, 0x2bc6, 0x2c82, 0x2daa, 0x2e66,
        0x3be2, 0x3c9a, 0x3d74, 0x3e3a, 0x3f12, 0x3ff6,
        0x40c6, 0x41ac, 0x42b6, 0x43ac, 0x44bc, 0x45d0
    };
    const uint16_t copy_y[] = {0x6941, 0x6041};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
        kuKernelCpuUnrestrictedMemcpy(
            (void *)((grid & ~(uintptr_t)1) + offsets[i]), copy_y, sizeof(copy_y));
}
