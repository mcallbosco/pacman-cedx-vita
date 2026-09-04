#include "game_afterimages.h"
#include "game_patch.h"
#include "settings.h"

#include <string.h>

static uintptr_t shadow_resume __attribute__((used));

/* Keep the native stack frame and softfp argument bits without rewriting
 * the entry each time the separate chain-tail trail emits a shadow. */
static void __attribute__((naked, noinline)) shadow_original(void *ghost, uint32_t distance) {
    __asm__(
        "push {r7, lr}\n"
        "mov r7, sp\n"
        "sub sp, #24\n"
        "vmov s0, r1\n"
        "ldr ip, =shadow_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

static void create_shadow(void *ghost, uint32_t distance) {
    /* The native method always updates the movement anchor before deciding
     * whether to create a visual task. Keep that side effect when disabled.
     * The separate chain-trail option can still show the native tail trail. */
    if (!setting_ghostAfterimages &&
        !((((const unsigned char *)ghost)[0x1f8] & 1) && setting_ghostChainTrails)) {
        memcpy((char *)ghost + 0x138, (const char *)ghost + 0xc4, 8);
        return;
    }
    shadow_original(ghost, distance);
}

void game_afterimages_install_hooks(void) {
    if (setting_ghostAfterimages)
        return;
    uintptr_t shadow = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask12CreateShadowEf", 0x98, 0x14f6fa85u);
    if (shadow) {
        shadow_resume = shadow + 0x0a;
        hook_addr(shadow, (uintptr_t)create_shadow);
    }
}
