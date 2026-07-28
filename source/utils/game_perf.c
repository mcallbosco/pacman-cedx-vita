#include "game_perf.h"
#include "game_patch.h"
#include "game_spacing.h"
#include "logger.h"
#include "settings.h"

#include <so_util/so_util.h>
#include <stdint.h>
#include <string.h>

extern so_module so_mod;

static so_hook train_index_hook;
/* Android libc++ vector: begin, end, capacity. Read live pointers on each call;
 * the train can be reordered or reallocated without changing its size. */
static void ***train_list;

static void **find_train_ghost(void *ghost) {
    void **begin = train_list[0];
    void **end = train_list[1];
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

    addr = game_patch_checked_function("_ZN3sys8ShEffect5ApplyEPNS_7cSpriteE",
                            0x1c4, 0xa91519f9u);
    if (addr) {
        /* +0x52..+0xae constructs a scaled centre and a rotation value in
         * stack temporaries that are never read. Skip to +0xb0, retaining all
         * shader selection, projection, colour and HSV uniform updates.
         * Thumb B at +0x52: PC=+0x56, displacement=0x5a (45 halfwords). */
        const uint16_t skip_unused_transform = 0xe02d;
        kuKernelCpuUnrestrictedMemcpy((void *)((addr & ~(uintptr_t)1) + 0x52),
                                     &skip_unused_transform, sizeof(skip_unused_transform));
    }
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
    reuse_sprite_corner_transforms();
    /* so_patch flushes the module's instruction cache after all hooks. */
}
