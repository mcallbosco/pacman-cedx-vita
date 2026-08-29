#include "game_perf.h"
#include "game_patch.h"
#include "game_spacing.h"
#include "game_transform.h"
#include "game_tasks.h"
#include "game_area.h"
#include "game_grid.h"
#include "game_map_color.h"
#include "game_texture.h"
#include "game_vertex.h"
#include "game_profile.h"
#include "game_wave.h"
#include "game_frame.h"
#include "game_ghost_scan.h"
#include "game_batch.h"
#include "game_math.h"
#include "game_shader.h"
#include "game_sprite_draw.h"
#include "game_viewport.h"
#include "game_navigation.h"
#include "game_fade.h"
#include "logger.h"
#include "settings.h"

#include <so_util/so_util.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>

extern so_module so_mod;

static so_hook train_index_hook;
/* Android libc++ vector: begin, end, capacity. Read live pointers on each call;
 * the train can be reordered or reallocated without changing its size. */
static void ***train_list;

static void **find_train_ghost(void *ghost) {
    void **begin = train_list[0];
    void **end = train_list[1];
    if (begin == end)
        return NULL;
    if (*begin == ghost)
        return begin;
    void **entry;
    if (end - begin >= 64 && game_ghost_scan_train(train_list, ghost, &entry))
        return entry;
    ++begin;
    const uint32x4_t target = vdupq_n_u32((uint32_t)(uintptr_t)ghost);
    /* Scan eight live entries at a time without reading beyond the vector.
     * Resolve a matching block in order to preserve first-match semantics. */
    while (end - begin >= 8) {
        uint32x4_t a = vld1q_u32((const uint32_t *)begin);
        uint32x4_t b = vld1q_u32((const uint32_t *)(begin + 4));
        uint32x4_t matches = vorrq_u32(vceqq_u32(a, target), vceqq_u32(b, target));
        uint32x2_t lanes = vorr_u32(vget_low_u32(matches), vget_high_u32(matches));
        if (vget_lane_u32(vpmax_u32(lanes, lanes), 0)) {
            for (size_t i = 0; i < 8; ++i)
                if (begin[i] == ghost)
                    return begin + i;
        }
        begin += 8;
    }
    for (void **it = begin; it != end; ++it) {
        if (*it == ghost)
            return it;
    }
    return NULL;
}

static int train_ghost_index(void *ghost) {
    void **entry = find_train_ghost(ghost);
    if (entry)
        return (int)(entry - train_list[0]);
    /* Preserve the game's assertion path for a ghost outside the train. */
    return SO_CONTINUE(int, train_index_hook, ghost);
}

static int is_train_ghost(void *ghost) {
    return find_train_ghost(ghost) != NULL;
}

static void *target_train_ghost(void *ghost) {
    void **entry = find_train_ghost(ghost);
    /* The train leader and ghosts outside the train have no predecessor.
     * Read the live vector so removals and reordering are visible immediately. */
    return entry && entry != train_list[0] ? entry[-1] : NULL;
}

/* Integer arguments preserve Android's soft-float calling convention and all
 * colour bits, including signed zero. SetColor only assigns Color4f at +0x78. */
static void *sprite_set_color(void *sprite, uint32_t r, uint32_t g,
                              uint32_t b, uint32_t a) {
    uint32_t color[4] = {r, g, b, a};
    void *dest = (char *)sprite + 0x78;
    memcpy(dest, color, sizeof(color));
    return dest;
}


static void reuse_sprite_corner_transforms(void) {
    uintptr_t draw = game_patch_checked_function(
        "_ZN3sys7cSprite10DrawNormalEv", 0x160c, 0x3f244666u);
    if (!draw || !game_patch_checked_function(
            "_ZN3sys7cSprite7ConvPosENS_8cVector2Eb", 0x18c, 0xd7692dceu))
        return;

    game_sprite_draw_install(draw);

    static const struct {
        uint16_t offset;
        uint16_t load_base;
        uint16_t load_offset;
    } corners[] = {
        {0x202, 0xed17, 0x0a2d}, /* [r7, #-180] */
        {0x32e, 0xed17, 0x0a37}, /* [r7, #-220] */
        {0x46c, 0xed9d, 0x0afd}, /* [sp, #1012] */
        {0x590, 0xed9d, 0x0af3}, /* [sp, #972] */
        {0x6d8, 0xed9d, 0x0ae9}, /* [sp, #932] */
        {0x7d8, 0xed9d, 0x0adf}, /* [sp, #892] */
        {0x8da, 0xed9d, 0x0ad5}, /* [sp, #852] */
        {0x9d4, 0xed9d, 0x0acb}, /* [sp, #812] */
        {0xae4, 0xed9d, 0x0ac1}, /* [sp, #772] */
        {0xbe6, 0xed9d, 0x0ab7}, /* [sp, #732] */
    };
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); ++i) {
        const uint16_t patch[] = {
            0xbf00, 0xbf00, corners[i].load_base, corners[i].load_offset
        };
        kuKernelCpuUnrestrictedMemcpy(
            (void *)((draw & ~(uintptr_t)1) + corners[i].offset), patch, sizeof(patch));
    }
}

void game_perf_install_hooks(void) {
    game_ghost_scan_install_hooks();
    uintptr_t addr = game_patch_checked_function("_ZN3sys7cSprite8SetColorEffff",
                                      0x8c, 0x78062d18u);
    if (addr)
        hook_addr(addr, (uintptr_t)sprite_set_color);

    addr = game_patch_checked_function("_ZN9newPacman12cOnGhostTask18getTrainGhostIndexEPS0_",
                            0xb4, 0x6e62ebb1u);
    train_list = (void ***)so_symbol(&so_mod, "_ZN9newPacman12cOnGhostTask10mTrainListE");
    if (addr && train_list)
        train_index_hook = hook_addr(addr, (uintptr_t)train_ghost_index);

    addr = game_patch_checked_function("_ZN9newPacman12cOnGhostTask12isTrainGhostEPS0_",
                                       0x70, 0x9f222769u);
    if (addr && train_list)
        hook_addr(addr, (uintptr_t)is_train_ghost);

    addr = game_patch_checked_function("_ZN9newPacman12cOnGhostTask19getTargetTrainGhostEPS0_",
                                       0xb8, 0x05e0dd79u);
    if (addr && train_list)
        hook_addr(addr, (uintptr_t)target_train_ghost);

    game_shader_install_hooks();
    if (setting_reduceGhostTrails) {
        addr = game_patch_checked_function("_ZN9newPacman12cOnGhostTask12OnModeNormalEv",
                                0x230, 0xbdb30a78u);
        if (addr) {
            /* Increase the distance between body afterimages from 1 to 2.
             * Keep CreateShadow's anchor updates and all movement logic.
             * Thumb VMOV.F32 s2,#2.0 replaces VMOV.F32 s2,#1.0 at +0x204. */
            const uint16_t spacing_two[] = {0xeeb0, 0x1a00};
            kuKernelCpuUnrestrictedMemcpy((void *)((addr & ~(uintptr_t)1) + 0x204),
                                         spacing_two, sizeof(spacing_two));
        }
    }
    game_spacing_install_hooks();
    game_viewport_install();
    reuse_sprite_corner_transforms();
    game_grid_install_hooks();
    game_transform_install_hooks();
    game_tasks_install_hooks();
    game_area_install_hooks();
    /* Batch guards inspect the original transform bodies before replacement. */
    game_batch_install_hooks();
    game_vertex_install_hooks();
    game_profile_install_hooks();
    game_wave_install_hooks();
    game_frame_install_hooks();
    game_map_color_install_hooks();
    game_texture_install_hooks();
    game_fade_install_hooks();
    game_navigation_install_hooks();
    game_math_install_hooks();
    /* so_patch flushes the module's instruction cache after all hooks. */
}
