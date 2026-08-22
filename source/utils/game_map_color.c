#include "game_map_color.h"
#include "game_patch.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static so_hook base_color_hook;
static const void *excel_params;
static const void *base_colors;
static int (*is_kurayami)(void);
static void *(*texture_manager_get)(void);
static const char *(*map_skin_get)(void *manager);
static uintptr_t map_hsv_resume __attribute__((used));

static uint32_t word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static void copy_rgb(void *target, const void *source) {
    for (size_t i = 0; i < 3; ++i) {
        uint32_t value = word(source, i * sizeof(value));
        memcpy((char *)target + i * sizeof(value), &value, sizeof(value));
    }
}

static void map_base_color(void *color) {
    int32_t selection = (int32_t)word(excel_params, 328);
    /* The supported table has five entries. Keep native behavior for an
     * unexpected selector rather than indexing beyond this replacement. */
    if (selection > 5) {
        so_hook_unpatch(&base_color_hook);
        ((void (*)(void *))base_color_hook.thumb_addr)(color);
        so_hook_repatch(&base_color_hook);
        return;
    }

    static const uint32_t choices[5][3] = {
        {0, 0, 0x3f800000},
        {0x3f000000, 0, 0x3f800000},
        {0x3f000000, 0x3f000000, 0x3f000000},
        {0x3f000000, 0x3f000000, 0},
        {0, 0x3f800000, 0}
    };
    copy_rgb(color, (const char *)base_colors + 16);
    if (selection >= 1)
        copy_rgb(color, choices[selection - 1]);
    /* The native callback runs after the starting RGB is written, and the
     * ending RGB is read afterward. Preserve that observable order. */
    int dark = is_kurayami();
    if (dark) {
        static const uint32_t black[3];
        copy_rgb(color, black);
    }
    copy_rgb((char *)color + 12, (const char *)base_colors + (dark ? 112 : 0));
}

/* Only the constant-color branch is replaced. Native dynamic hue/value
 * overrides, interpolation and rate-dependent setup still run around it. */
static void __attribute__((used, noinline)) map_constant_hsv(
        const void *color, void *result) {
    static const uint32_t hsv[3][3] = {
        {0, 0, 0x3f800000},
        {0x43960000, 0x3f400000, 0x3f800000},
        {0x43700000, 0x3e95f6fe, 0x3f800000}
    };
    float rate;
    memcpy(&rate, (const char *)color + 24, sizeof(rate));
    unsigned choice = 0;
    if (rate != 0.0f) {
        const char *skin = map_skin_get(texture_manager_get());
        /* The native first comparison selects pink unless the name is
         * skinA. Its following skinB comparison then selects blue. */
        choice = strcmp(skin, "skinA") ? 1 : 2;
    }
    copy_rgb(result, hsv[choice]);
}

static void __attribute__((naked)) map_hsv_bridge(void) {
    __asm__ volatile(
        "ldr r0, [sp, #68]\n"
        "ldr r1, [sp, #100]\n"
        "bl map_constant_hsv\n"
        "ldr ip, =map_hsv_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_map_color_install_hooks(void) {
    uintptr_t base = game_patch_checked_function(
        "_ZN9newPacman11cTsMapColor12SetBaseColorEv", 0x1f8, 0x3427ffb8u);
    uintptr_t hsv = game_patch_checked_function(
        "_ZN9newPacman11cTsMapColor9SetMapHSVEPN3sys8cVector4E", 0x3c8, 0xce45fdb2u);
    if (!game_patch_checked_function("_ZN3sys7Color4fC2Ev", 0x18, 0xa5d96f56u) ||
        !game_patch_checked_function("_ZN3sys7Color4fC2Effff", 0x4a, 0xcb71b543u) ||
        !game_patch_checked_function("_ZN3sys7Color4faSERKS0_", 0x3c, 0xfb314696u))
        return;

    excel_params = (void *)so_symbol(&so_mod, "_ZN9newPacman14cOnExcelParams1pE");
    base_colors = (void *)so_symbol(&so_mod, "_ZN3sys8cPacTune12MapBaseColorE");
    is_kurayami = (void *)game_patch_checked_function(
        "_ZN9newPacman11CPacmanGame14IsKurayamiModeEv", 0x14, 0xf9bce6acu);
    if (base && excel_params && base_colors && is_kurayami)
        base_color_hook = hook_addr(base, (uintptr_t)map_base_color);

    texture_manager_get = (void *)game_patch_checked_function(
        "_ZN3sys7cCommon17TextureManagerGetEv", 0x10, 0x0585419au);
    map_skin_get = (void *)game_patch_checked_function(
        "_ZN3sys14TextureManager17GetCurMapSkinInfoEv", 0x1a, 0x4a46596bu);
    if (!hsv || !texture_manager_get || !map_skin_get ||
        !game_patch_checked_function("_ZN3sys5cMath7Rgb2HsvEPfS1_S1_fff", 0x1c8, 0x0a5b63efu) ||
        !game_patch_checked_function("_ZN3sys5cMath9max_colorEfff", 0x7e, 0x97c525c1u) ||
        !game_patch_checked_function("_ZN3sys5cMath9min_colorEfff", 0x7e, 0x67a8ab49u))
        return;

    uintptr_t code = hsv & ~(uintptr_t)1;
    /* These read-only strings explain the native constant-color branches. */
    if (strcmp((const char *)(code + 0x1cc + word((void *)code, 0x3b0)), "%s") ||
        strcmp((const char *)(code + 0x1e2 + word((void *)code, 0x3b4)), "skinA") ||
        strcmp((const char *)(code + 0x244 + word((void *)code, 0x3b8)), "skinB"))
        return;
    map_hsv_resume = hsv + 0x338;
    hook_addr(hsv + 0x15a, (uintptr_t)map_hsv_bridge);
}
