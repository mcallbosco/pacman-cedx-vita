#include "game_ghost_eat.h"
#include "game_patch.h"

#include <math.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static void (*wave_set_color)(void *, float, float, float, float);
static uintptr_t wave_color_resume __attribute__((used));

static void __attribute__((used, noinline)) ghost_wave_color(void *wave, float alpha) {
    void *sprite = (char *)wave + 28;
    const char *name;
    memcpy(&name, (const char *)sprite + 4, sizeof(name));
    if (name && !strcmp(name, "pac_ce_eff02")) {
        float progress;
        memcpy(&progress, (const char *)wave + 0xd0, sizeof(progress));
        /* Native waves kept alpha at one for their entire normal lifetime.
         * Give the outline a translucent start and fade over its expansion. */
        alpha = isfinite(progress) ? 0.35f * (1.0f - progress / 1.05f) : 0.0f;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 0.35f) alpha = 0.35f;
    }
    wave_set_color(sprite, 1.0f, 1.0f, 1.0f, alpha);
}

/* Replace the color call, retaining the wave's scale, position and lifetime. */
static void __attribute__((naked)) ghost_wave_color_bridge(void) {
    __asm__ volatile(
        "ldr r0, [sp, #24]\n"
        "ldr r1, [sp, #8]\n"
        "push {r4, lr}\n"
        "bl ghost_wave_color\n"
        "pop {r4, lr}\n"
        "ldr r0, [sp, #24]\n"
        "ldr ip, =wave_color_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

static void install_outline_fade(void) {
    uintptr_t wave = game_patch_checked_function(
        "_ZN9newPacman11cTsTaskWave4FuncEv", 0xd4, 0x876771b9u);
    wave_set_color = (void *)game_patch_checked_function(
        "_ZN3sys7cSprite8SetColorEffff", 0x8c, 0x78062d18u);
    if (!wave || !wave_set_color || !game_patch_checked_function(
            "_ZN3sys10cPrimitive7SetNameEPKc", 0x18, 0x77ca1ed6u))
        return;
    wave_color_resume = wave + 0xac;
    hook_addr(wave + 0xa4, (uintptr_t)ghost_wave_color_bridge);
}

void game_ghost_eat_install_hooks(void) {
    uintptr_t effect = game_patch_checked_function(
        "_ZN9newPacman21cTsTaskEffectGhostEat4FuncEv", 0x3b4, 0x979f73fcu);
    if (!effect || !game_patch_checked_function(
            "_ZN9newPacman11cTsTaskWaveC2EiPfS1_if", 0x1dc, 0x37c48871u))
        return;
    /* This private child handles the delayed ghost score/wave animation.
     * Its vtable is constructed by the checked ghost-eating effect. */
    uintptr_t child = game_patch_checked_code(effect + 0xe5c,
        "ghost-eating wave child", 0x19c, 0x6f7ff0a4u);
    if (!child)
        return;
    uintptr_t code = child & ~(uintptr_t)1;
    /* Select the existing Pac-Man outline (type 1, resource 19) in place of
     * the circle (type 0, resource 20). The native sprite handles its size,
     * expansion, fade, position tracking and task lifetime. */
    const uint16_t outline = 0x2101; /* movs r1, #1 */
    kuKernelCpuUnrestrictedMemcpy((void *)(code + 0x80), &outline, sizeof(outline));
    /* When that wave was emitted, bypass the optional second outline.
     * Keep its original conditions when the primary wave was suppressed. */
    const uint16_t once = 0xe036; /* b 0x1e5260: resume the native score delay */
    kuKernelCpuUnrestrictedMemcpy((void *)(code + 0x88), &once, sizeof(once));
    install_outline_fade();
}
