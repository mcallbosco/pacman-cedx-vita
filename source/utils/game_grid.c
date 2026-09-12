#include "game_grid.h"
#include "game_patch.h"
#include "game_map_draw.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math")

static so_hook grid_color_hook;
static void **game_model_instance;
static const uint8_t *darker_map;
static uintptr_t grid_resume __attribute__((used));
static void *(*grid_transform)(void *, const void *, const void *, uint32_t);
static void **graphics_instance;
static uintptr_t viewport_transform;

typedef struct {
    uint32_t point[2], result[2];
} GridPosition;

typedef struct GridCache {
    uint32_t count, next;
    GridPosition positions[4];
} GridCache;

static GridCache *grid_cache;

/* These instructions are the checked DrawGrid prologue, before any PC-relative
 * accesses. Resume after it without rewriting code on each draw. */
static void __attribute__((naked)) grid_original(void *sprite) {
    __asm__ volatile(
        "push {r4, r6, r7, lr}\n"
        "add r7, sp, #8\n"
        "sub.w sp, sp, #3456\n"
        "ldr ip, =grid_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

static void draw_grid(void *sprite) {
    GridCache cache;
    GridCache *previous = grid_cache;
    cache.count = cache.next = 0;
    grid_cache = &cache;
    /* The native continuation calls back into grid_position outside the
     * compiler's visible call graph. Publish and retain this stack context. */
    __asm__ volatile("" ::: "memory");
    grid_original(sprite);
    __asm__ volatile("" ::: "memory");
    grid_cache = previous;
}

/* Only DrawGrid's transform calls enter here. Shared positions retain distinct
 * UV/colour vertices and every native viewport callback still executes. The
 * checked native loop never writes the sprite, uses one unscaled flag, and
 * passes distinct stack-local inputs and outputs. Only its viewport callback
 * can change transform state between vertices: accept the verified read-only
 * callback or no callback, and invalidate before any unfamiliar callback. */
static void *grid_position(void *result, const void *sprite,
                            const void *point, uint32_t unscaled) {
    GridCache *cache = grid_cache;
    uintptr_t callback;
    memcpy(&callback, (const char *)*graphics_instance + 0x14, sizeof(callback));
    if (!cache || (callback && callback != viewport_transform)) {
        if (cache)
            cache->count = cache->next = 0;
        return grid_transform(result, sprite, point, unscaled);
    }
    uint32_t xy[2];
    memcpy(xy, point, sizeof(xy));
    for (uint32_t i = 0; i < 4 && i < cache->count; ++i) {
        const GridPosition *entry = &cache->positions[i];
        if (entry->point[0] == xy[0] && entry->point[1] == xy[1]) {
            memcpy(result, entry->result, sizeof(entry->result));
            return result;
        }
    }
    grid_transform(result, sprite, point, unscaled);
    GridPosition *entry = &cache->positions[cache->next];
    memcpy(entry->point, xy, sizeof(xy));
    memcpy(entry->result, result, sizeof(entry->result));
    cache->next = (cache->next + 1) & 3;
    if (cache->count < 4)
        ++cache->count;
    return result;
}

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

static void install_position_reuse(uintptr_t grid, uintptr_t transform) {
    if (!game_patch_checked_function("_ZN3sys5cMath4HalfEf", 0x6e, 0xbfb8c072u) ||
        !game_patch_checked_function("_ZN3sys5cMath7sincosfEfPfS1_", 0x36, 0xe4e2be44u))
        return;

    graphics_instance = (void **)so_symbol(&so_mod, "_ZN3sys11g_pGraphicsE");
    viewport_transform = game_patch_checked_function(
        "_ZN3sys5runny8Renderer36TranformPointFromGlobalRunnyRendererERfS2_",
        0x90, 0xcf9c3747u);
    if (!graphics_instance || !viewport_transform ||
        !game_patch_checked_function(
            "_ZN3sys5runny8Renderer14TransformPointERKNS_8cVector2E",
            0xc4, 0x4081c677u) ||
        !game_patch_checked_function(
            "_ZN3sys4math6Matrix16TransformPoint2fEPfS2_",
            0xd8, 0x806f0c63u))
        return;

    static const uint16_t calls[] = {
        0x3d4, 0x484, 0x52e, 0x5c8, 0x670, 0x718,
        0x7bc, 0x88e, 0x970, 0xa42, 0xb22, 0xc0c,
        0x1cf6, 0x1dc2, 0x1eac, 0x1f88, 0x2070, 0x2164,
        0x2408, 0x24c0, 0x25d8, 0x2690, 0x27c8, 0x2884,
        0x29a0, 0x2a5c, 0x2b94, 0x2c50, 0x2d78, 0x2e34,
        0x3bbe, 0x3c76, 0x3d50, 0x3e16, 0x3eee, 0x3fd2,
        0x4094, 0x417a, 0x4284, 0x437a, 0x448a, 0x459e
    };
    uintptr_t code = grid & ~(uintptr_t)1;
    uintptr_t veneer = so_alloc_arena(&so_mod, 0x00ff0000, code, 8);
    if (!veneer)
        return;
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i) {
        int64_t delta = (int64_t)veneer - (int64_t)(code + calls[i] + 4);
        if (delta < -0x1000000 || delta > 0xfffffe || (delta & 1))
            return;
    }
    grid_transform = (void *)transform;
    grid_resume = grid + 8;
    hook_addr(veneer | 1, (uintptr_t)grid_position);
    kuKernelFlushCaches((void *)veneer, 8);
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i) {
        uint32_t delta = (uint32_t)(veneer - (code + calls[i] + 4));
        uint32_t sign = (delta >> 24) & 1;
        uint32_t j1 = (~(delta >> 23) ^ sign) & 1;
        uint32_t j2 = (~(delta >> 22) ^ sign) & 1;
        const uint16_t branch[] = {
            0xf000 | (sign << 10) | ((delta >> 12) & 0x3ff),
            0xd000 | (j1 << 13) | (j2 << 11) | ((delta >> 1) & 0x7ff)
        };
        kuKernelCpuUnrestrictedMemcpy((void *)(code + calls[i]), branch, sizeof(branch));
    }
    hook_addr(grid, (uintptr_t)draw_grid);
}

void game_grid_install_hooks(void) {
    install_grid_color();
    uintptr_t grid = game_patch_checked_function(
        "_ZN3sys7cSprite8DrawGridEv", 0x4ec4, 0x269aca4cu);
    uintptr_t transform = game_patch_checked_function(
        "_ZN3sys7cSprite7ConvPosENS_8cVector2Eb", 0x18c, 0xd7692dceu);
    if (!grid || !transform)
        return;
    game_map_draw_install(grid);

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
    install_position_reuse(grid, transform);
}
