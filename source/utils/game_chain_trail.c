#include "game_chain_trail.h"
#include "game_patch.h"
#include "settings.h"

#include <math.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static uintptr_t chain_trail_resume __attribute__((used));

static void __attribute__((used, noinline)) chain_trail_tint(void *shadow) {
    void *sprite;
    float hue;
    memcpy(&sprite, (const char *)shadow + 0xc0, sizeof(sprite));
    memcpy(&hue, (const char *)sprite + 0x94, sizeof(hue));
    if (!isfinite(hue) || hue < 0.0f || hue > 1.0f)
        return;

    /* Android's train-shadow frame is grayscale. Approximate the PC frame's
     * purple base hue and saturation, then apply its existing animated hue.
     * Tint once at creation; the native update retains its opacity and fade. */
    hue += 0.756f;
    if (hue >= 1.0f)
        hue -= 1.0f;
    float phase = hue * 6.0f;
    float rgb[] = {fabsf(phase - 3.0f) - 1.0f,
                   2.0f - fabsf(phase - 2.0f),
                   2.0f - fabsf(phase - 4.0f)};
    for (unsigned i = 0; i < 3; ++i) {
        if (rgb[i] < 0.0f) rgb[i] = 0.0f;
        if (rgb[i] > 1.0f) rgb[i] = 1.0f;
        rgb[i] = 255.0f * (0.27f + 0.73f * rgb[i]);
    }
    memcpy((char *)shadow + 0xd8, rgb, sizeof(rgb));
}

/* Only the unfrightened train branch reaches this point, after storing hue.
 * Replay the native alpha write, then tint its existing shadow. No shader or
 * sprite-count changes are needed, including in low-performance mode. */
static void __attribute__((naked)) chain_trail_bridge(void) {
    __asm__ volatile(
        "mov.w r0, #0x43000000\n"
        "ldr r1, [sp, #60]\n"
        "str r0, [r1, #228]\n"
        "push {r0-r3, ip, lr}\n"
        "mov r0, r1\n"
        "bl chain_trail_tint\n"
        "pop {r0-r3, ip, lr}\n"
        "ldr ip, =chain_trail_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_chain_trail_install_hooks(void) {
    if (!setting_ghostChainTrails)
        return;
    uintptr_t ctor = game_patch_checked_function(
        "_ZN9newPacman13cOnShadowTaskC2EPNS_12cOnGhostTaskE", 0x344, 0xc6dea3afu);
    if (!ctor || !game_patch_checked_function(
            "_ZN9newPacman13cOnShadowTask4FuncEv", 0x130, 0xa5f0fda7u))
        return;
    chain_trail_resume = ctor + 0x178;
    hook_addr(ctor + 0x16e, (uintptr_t)chain_trail_bridge);
}
