#include "game_perf.h"
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

static int train_ghost_index(void *ghost) {
    void **begin = train_list[0];
    void **end = train_list[1];
    for (void **it = begin; it != end; ++it) {
        if (*it == ghost)
            return (int)(it - begin);
    }
    /* Preserve the game's assertion path for a ghost outside the train. */
    return SO_CONTINUE(int, train_index_hook, ghost);
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

/* FNV-1a fingerprints cover the entire original function, including its literal
 * pool. These layout-dependent patches were checked against Android 1.2.0's
 * armeabi-v7a library (Build ID 282a7990ef34572c6fdcea7913840b11feaca95e).
 * A different function body is left untouched. This is a version check, not an
 * integrity/security check. */
static uintptr_t checked_function(const char *symbol, size_t size, uint32_t hash) {
    uintptr_t addr = so_symbol(&so_mod, symbol);
    uintptr_t code = addr & ~(uintptr_t)1;
    if (!(addr & 1) || code < so_mod.text_base || size > so_mod.text_size ||
        code - so_mod.text_base > so_mod.text_size - size)
        return 0;

    const uint8_t *bytes = (const uint8_t *)code;
    uint32_t actual = 2166136261u;
    for (size_t i = 0; i < size; ++i)
        actual = (actual ^ bytes[i]) * 16777619u;
    if (actual != hash) {
        l_warn("Performance patch skipped: unexpected code for %s", symbol);
        return 0;
    }
    return addr;
}

void game_perf_install_hooks(void) {
    uintptr_t addr = checked_function("_ZN3sys7cSprite8SetColorEffff",
                                      0x8c, 0x78062d18u);
    if (addr)
        hook_addr(addr, (uintptr_t)sprite_set_color);

    addr = checked_function("_ZN9newPacman12cOnGhostTask18getTrainGhostIndexEPS0_",
                            0xb4, 0x6e62ebb1u);
    train_list = (void ***)so_symbol(&so_mod, "_ZN9newPacman12cOnGhostTask10mTrainListE");
    if (addr && train_list)
        train_index_hook = hook_addr(addr, (uintptr_t)train_ghost_index);

    addr = checked_function("_ZN3sys8ShEffect5ApplyEPNS_7cSpriteE",
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
        addr = checked_function("_ZN9newPacman12cOnGhostTask12OnModeNormalEv",
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
    /* so_patch flushes the module's instruction cache after all hooks. */
}
