#include "game_grid.h"
#include "game_patch.h"

void game_grid_install_hooks(void) {
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
