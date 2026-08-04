#include "game_spacing.h"
#include "game_patch.h"
#include "game_ghost_scan.h"
#include <string.h>

/* Preserve the game's float comparisons, division and rounding order. */
#pragma GCC optimize ("no-fast-math")

static const void *spacing_params;
static void *(*child_begin)(void *parent);
static void *(*child_end)(void *parent);
static uintptr_t spacing_resume __attribute__((used));

/* Fields from Android 1.2.0. memcpy avoids aliasing native C++ objects. */
static uint32_t word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static float number(const void *object, size_t offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static int16_t direction(const void *ghost) {
    int16_t value;
    memcpy(&value, (const char *)ghost + 0x104, sizeof(value));
    return value;
}

static int nearby(const void *a, const void *b, float limit, int same_direction) {
    if (((const unsigned char *)a)[0xed] || ((const unsigned char *)b)[0xed])
        return 0;
    int dir = direction(a);
    if (same_direction && dir != direction(b))
        return 0;
    float distance;
    if ((dir == 1 || dir == 3) && number(a, 0xc4) == number(b, 0xc4)) {
        distance = (number(a, 0xc8) - number(b, 0xc8)) * (dir == 1 ? -1.0f : 1.0f);
        return distance >= 0.0f && distance < limit;
    }
    if ((dir == 0 || dir == 2) && number(a, 0xc8) == number(b, 0xc8)) {
        distance = (number(a, 0xc4) - number(b, 0xc4)) * (dir == 2 ? -1.0f : 1.0f);
        return distance >= 0.0f && distance < limit;
    }
    return 0;
}

/* 1 is the train multiplier, 2 the spacing divisor. Test the train rule first
 * for a sibling which satisfies both, matching the original list traversal. */
int game_spacing_rule(void *ghost, void *other, int *near_train) {
    if ((((const unsigned char *)other)[0x1f8] & 1) &&
        (nearby(other, ghost, 2.0f, 0) || nearby(ghost, other, 2.0f, 0))) {
        *near_train = 1;
        int32_t counter;
        uint32_t bits = word(ghost, 0x118);
        memcpy(&counter, &bits, sizeof(counter));
        if ((float)counter > number(spacing_params, 0x198) * 60.0f)
            return 1;
    }
    return nearby(other, ghost, 1.0f, 1) ? 2 : 0;
}

/* Replace only getSpeed's sibling scan. Keep its surrounding Pac-Man proximity,
 * train-following, power-up and slow-motion calculations in the original code. */
static uint32_t __attribute__((used, noinline)) spacing_speed(void *ghost, uint32_t speed_bits) {
    float speed;
    memcpy(&speed, &speed_bits, sizeof(speed));
    int near_train = 0;
    int rule = game_ghost_scan_spacing(ghost, &near_train);
    if (rule < 0) {
        void *parent = (void *)(uintptr_t)word(ghost, 4);
        void *end = child_end(parent);
        rule = 0;
        for (void *node = child_begin(parent); node != end;
             node = (void *)(uintptr_t)word(node, 4)) {
            void *other = (void *)(uintptr_t)word(node, 8);
            if (other != ghost && (rule = game_spacing_rule(ghost, other, &near_train)))
                break;
        }
    }
    if (rule == 1) {
        speed *= number(spacing_params, 0x278);
        goto adjusted;
    }
    if (rule == 2) {
        speed /= 1.5f;
        goto adjusted;
    }
    /* Early speed adjustments intentionally leave this counter unchanged. */
    uint32_t counter = near_train ? word(ghost, 0x118) + 1u : 0u;
    memcpy((char *)ghost + 0x118, &counter, sizeof(counter));
adjusted:
    memcpy(&speed_bits, &speed, sizeof(speed_bits));
    return speed_bits;
}

/* At the checked +0xf4 site, getSpeed has a 152-byte frame with the ghost
 * pointer at SP+68 and current speed at SP+104. Preserve its frame and return
 * directly to +0x214; the function's own prologue/epilogue stay in control. */
static void __attribute__((naked)) spacing_bridge(void) {
    __asm__ volatile(
        "ldr r0, [sp, #68]\n"
        "ldr r1, [sp, #104]\n"
        "push {r4, lr}\n"
        "bl spacing_speed\n"
        "pop {r4, lr}\n"
        "str r0, [sp, #104]\n"
        "ldr ip, =spacing_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_spacing_install_hooks(void) {
    uintptr_t speed = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask8getSpeedEv", 0x630, 0xc21b3ea9u);
    if (!speed || !game_patch_checked_code(speed - 0x42c, "ghost spacing predicate",
                                           0x17c, 0xd65f94e0u))
        return;
    child_begin = (void *)game_patch_checked_function(
        "_ZN3sys5cTask17GetChildTaskBeginEv", 0x1c, 0x31ea37d6u);
    child_end = (void *)game_patch_checked_function(
        "_ZN3sys5cTask15GetChildTaskEndEv", 0x1c, 0xf94903a9u);
    spacing_params = (void *)so_symbol(&so_mod, "_ZN9newPacman14cOnExcelParams1pE");
    if (!child_begin || !child_end || !spacing_params)
        return;
    spacing_resume = speed + 0x214;
    hook_addr(speed + 0xf4, (uintptr_t)spacing_bridge);
}
