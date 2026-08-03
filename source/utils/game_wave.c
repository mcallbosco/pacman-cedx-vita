#include "game_wave.h"
#include "game_patch.h"

static uintptr_t wave_resume __attribute__((used));

/* The original index-to-row/column-to-index round trip is exactly linear.
 * Retain the native wave physics, flags and loop, changing only point lookup. */
static void __attribute__((naked)) wave_point_bridge(void) {
    __asm__ volatile(
        "ldr r0, [sp, #72]\n"
        "ldr r0, [r0, #32]\n"
        "ldr r0, [r0, #28]\n"
        "ldr r1, [sp, #80]\n"
        "movs r2, #52\n"
        "mla r0, r1, r2, r0\n"
        "str r0, [sp, #88]\n"
        "ldr ip, =wave_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_wave_install_hooks(void) {
    uintptr_t wave = game_patch_checked_function(
        "_ZN9newPacman21cTsTaskEffectWaveCalc4FuncEv", 0x278, 0x02053483u);
    if (!wave ||
        !game_patch_checked_function("_ZN3sys7cSprite12GetGridPointEjj", 0x38, 0xb6c115cfu) ||
        !game_patch_checked_function("_ZN3sys7cSprite17GetGridPointWidthEv", 0x12, 0xc34fd6d3u))
        return;
    wave_resume = wave + 0xae;
    hook_addr(wave + 0x60, (uintptr_t)wave_point_bridge);
}
