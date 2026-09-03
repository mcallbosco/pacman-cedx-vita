#include "game_afterimages.h"
#include "game_patch.h"
#include "settings.h"

#include <string.h>

static so_hook shadow_hook;

static void create_shadow(void *ghost, float distance) {
    /* The native method always updates the movement anchor before deciding
     * whether to create a visual task. Keep that side effect when disabled.
     * The separate chain-trail option can still show the native tail trail. */
    if (!setting_ghostAfterimages &&
        !((((const unsigned char *)ghost)[0x1f8] & 1) && setting_ghostChainTrails)) {
        memcpy((char *)ghost + 0x138, (const char *)ghost + 0xc4, 8);
        return;
    }
    so_hook_unpatch(&shadow_hook);
    ((void (*)(void *, float))shadow_hook.thumb_addr)(ghost, distance);
    so_hook_repatch(&shadow_hook);
}

void game_afterimages_install_hooks(void) {
    if (setting_ghostAfterimages)
        return;
    uintptr_t shadow = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask12CreateShadowEf", 0x98, 0x14f6fa85u);
    if (shadow)
        shadow_hook = hook_addr(shadow, (uintptr_t)create_shadow);
}
