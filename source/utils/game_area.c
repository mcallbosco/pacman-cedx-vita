#include "game_area.h"
#include "game_patch.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math")

static const void *area_params;

static float number(const void *object, size_t offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static float negate_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bits ^= 0x80000000u;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void update_train_area(void *ghost, int32_t elapsed) {
    float width, half_height;
    if (((const unsigned char *)ghost)[0xec] == 4) {
        half_height = number(area_params, 0x164);
        width = half_height + half_height;
    } else {
        width = number(area_params, 0x158);
        half_height = number(area_params, 0x15c) / 2.0f;
        float seconds = (float)elapsed / 60.0f;
        float factor = 0.0f;
        for (size_t i = 0; i < 10; ++i) {
            if (!(seconds > number(area_params, 0x1a8 + 4 * i)))
                break;
            factor = number(area_params, 0x1d0 + 4 * i);
        }
        if (factor > 1.0f)
            factor = 1.0f;
        width *= factor;
        half_height *= factor;
    }
    /* Preserve the native Rect(x,y,width,height) layout and sign-bit negation.
     * Compute the shared dimensions once and write the four rectangles
     * directly, without temporary constructors and repeated copies. */
    float height = half_height + half_height;
    float neg_width = negate_bits(width), neg_half = negate_bits(half_height);
    const float area[] = {
        width, half_height,
        0.0f, neg_half, width, height,
        neg_half, neg_width, height, width,
        neg_width, neg_half, width, height,
        neg_half, 0.0f, height, width
    };
    memcpy((char *)ghost + 0x1b0, area, sizeof(area));
}

void game_area_install_hooks(void) {
    uintptr_t update = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask20updateGhostTrainAreaEi", 0x258, 0x13a87547u);
    if (!update || !game_patch_checked_function(
            "_ZN9newPacman4RectC2Effff", 0x4a, 0xcb71b543u))
        return;
    area_params = (void *)so_symbol(&so_mod, "_ZN9newPacman14cOnExcelParams1pE");
    if (area_params)
        hook_addr(update, (uintptr_t)update_train_area);
}
