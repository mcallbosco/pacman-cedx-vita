#include "game_eye_trails.h"
#include "game_patch.h"
#include "settings.h"

#include <math.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

enum { TRAIL_CAPACITY = 512, TRAIL_LIFE = 10 };
typedef struct {
    float x, y, alpha;
    unsigned born;
} EyeTrail;

/* Snapshot positions and opacity: trails never retain a ghost task pointer. */
static EyeTrail trails[TRAIL_CAPACITY];
static unsigned trail_first, trail_count, trail_tick;
static void **trail_sprite;
static int (*ghost_is_eye)(void *ghost);
static int (*trail_is_game)(void);
static int (*trail_is_preview)(void);
static void (*particle_add)(void *sprite, const float *position, const float *color);
static void (*sprite_submit)(void *sprite, void *list);
static float (*wall_to_x)(float), (*wall_to_y)(float);
static so_hook trail_reset_hook;
static uintptr_t trail_eye_resume __attribute__((used));
static uintptr_t trail_other_resume __attribute__((used));

static float number(const void *object, unsigned offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static uint32_t word(const void *object, unsigned offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static void eye_trails_reset(void) {
    trail_first = trail_count = trail_tick = 0;
    /* Keep native group-sprite setup, texture, blend mode and depth. */
    so_hook_unpatch(&trail_reset_hook);
    ((void (*)(void))trail_reset_hook.thumb_addr)();
    so_hook_repatch(&trail_reset_hook);
}

static void eye_trails_update(void) {
    ++trail_tick;
    /* Equal lifetimes keep the ring in expiry order. Unsigned subtraction
     * also preserves age when the tick counter wraps. */
    while (trail_count && trail_tick - trails[trail_first].born >= TRAIL_LIFE) {
        trail_first = (trail_first + 1) % TRAIL_CAPACITY;
        --trail_count;
    }
}

static void eye_trails_draw(void) {
    if (!setting_ghostEyeTrails || !trail_is_game() || trail_is_preview()) {
        trail_first = trail_count = 0;
        return;
    }
    void *sprite = *trail_sprite;
    /* This dedicated group is cleared after LoopDraw. Submit it once per frame. */
    if (!sprite || !word(sprite, 44) || word(sprite, 48))
        return;
    for (unsigned i = 0; i < trail_count; ++i) {
        /* ParticalQuadCentreAdd asserts when count + 1 reaches capacity. */
        unsigned used = word(sprite, 48), capacity = word(sprite, 52);
        if (capacity < 2 || used >= capacity - 1)
            break;
        const EyeTrail *trail = &trails[(trail_first + i) % TRAIL_CAPACITY];
        float fade = (float)(TRAIL_LIFE - (trail_tick - trail->born)) / TRAIL_LIFE;
        float position[] = {trail->x, trail->y};
        /* The native eye controller uses this blue tint and ten-tick fade. */
        float color[] = {0.2f, 0.8f * fade, 1.0f, trail->alpha * fade};
        particle_add(sprite, position, color);
    }
    if (word(sprite, 48))
        sprite_submit(sprite, NULL);
}

static int __attribute__((used, noinline)) eye_trails_emit(void *ghost) {
    int is_eye = ghost_is_eye(ghost);
    if (!is_eye || !setting_ghostEyeTrails || !trail_is_game() || trail_is_preview() ||
        trail_count == TRAIL_CAPACITY || (int32_t)word(ghost, 0x108) > 0)
        return is_eye;
    void *sprite = *trail_sprite;
    if (!sprite || !word(sprite, 44))
        return is_eye;
    float dx = number(ghost, 0x170), dy = number(ghost, 0x174);
    float x = number(ghost, 0x194), y = number(ghost, 0x198);
    float alpha = number(ghost, 0x110);
    float width = number(sprite, 92), height = number(sprite, 96);
    if (!isfinite(dx) || !isfinite(dy) || (dx == 0.0f && dy == 0.0f) ||
        !isfinite(x) || !isfinite(y) || !isfinite(alpha) || alpha <= 0.0f ||
        !isfinite(width) || !isfinite(height) || width <= 1.0f || height <= 1.0f)
        return is_eye;
    /* PC fills the motion path at 0.2 maze-unit intervals and skips jumps over
     * four units. Native dx/dy are this update's actual maze displacement. */
    float distance2 = dx * dx + dy * dy;
    if (!isfinite(distance2) || distance2 > 16.0f)
        return is_eye;
    float start_x = wall_to_x(number(ghost, 0xc4) - dx);
    float start_y = wall_to_y(number(ghost, 0xc8) - dy);
    if (!isfinite(start_x) || !isfinite(start_y))
        return is_eye;
    float intervals = sqrtf(distance2) / 0.2f;
    unsigned steps = (unsigned)intervals;
    if ((float)steps < intervals)
        ++steps;
    if (!steps)
        return is_eye;
    for (unsigned i = 1; i <= steps && trail_count < TRAIL_CAPACITY; ++i) {
        float t = (float)i / steps;
        EyeTrail *trail = &trails[(trail_first + trail_count) % TRAIL_CAPACITY];
        trail->x = start_x + (x - start_x) * t - width * 0.5f;
        trail->y = start_y + (y - start_y) * t - height * 0.5f;
        trail->alpha = alpha > 1.0f ? 1.0f : alpha;
        trail->born = trail_tick;
        ++trail_count;
    }
    return is_eye;
}

/* Replace one IsEye call after the native update has stored sprite coordinates.
 * Reproduce its comparison/branch; the original eye-visibility writes remain. */
static void __attribute__((naked)) eye_trails_bridge(void) {
    __asm__ volatile(
        "push {r4, lr}\n"
        "bl eye_trails_emit\n"
        "pop {r4, lr}\n"
        "cmp r0, #0\n"
        "beq 1f\n"
        "ldr ip, =trail_eye_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n"
        "1: ldr ip, =trail_other_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_eye_trails_install_hooks(void) {
    if (!setting_ghostEyeTrails)
        return;
    uintptr_t ghost = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask4FuncEv", 0x934, 0x15535a5eu);
    uintptr_t reset = game_patch_checked_function(
        "_ZN9newPacman16cOnEyeShadowCtrl5ResetEv", 0x7c, 0x06aa23f5u);
    uintptr_t update = game_patch_checked_function(
        "_ZN9newPacman16cOnEyeShadowCtrl6UpdateEv", 0x100, 0xc1baf529u);
    uintptr_t draw = game_patch_checked_function(
        "_ZN9newPacman16cOnEyeShadowCtrl4DrawEv", 0x134, 0x3f68eb98u);
    ghost_is_eye = (void *)game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask5IsEyeEv", 0x98, 0xe3f1ac87u);
    trail_is_game = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence6IsGameEv", 0x14, 0x476f8a74u);
    trail_is_preview = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence9IsPreviewEv", 0x14, 0x1128e4ffu);
    particle_add = (void *)game_patch_checked_function(
        "_ZN3sys7cSprite21ParticalQuadCentreAddERKNS_8cVector2ERKNS_7Color4fE",
        0x10c, 0x5df60236u);
    sprite_submit = (void *)game_patch_checked_function(
        "_ZN3sys7cSprite12AddPrimitiveEPNS_14cPrimitiveListE", 0x148, 0x8462da4cu);
    wall_to_x = (void *)game_patch_checked_function(
        "_ZN9newPacman9cOnScreen13WallX2SpriteXEf", 0x38, 0xa070945du);
    wall_to_y = (void *)game_patch_checked_function(
        "_ZN9newPacman9cOnScreen13WallY2SpriteYEf", 0x40, 0xf1d3ebbfu);
    trail_sprite = (void *)so_symbol(&so_mod,
        "_ZN9newPacman16cOnEyeShadowCtrl8spSpriteE");
    if (!ghost || !reset || !update || !draw || !ghost_is_eye || !trail_is_game ||
        !trail_is_preview || !particle_add || !sprite_submit || !wall_to_x ||
        !wall_to_y || !trail_sprite ||
        !game_patch_checked_function("_ZN9newPacman8LoopFuncEv", 0xc4, 0xdf9819cau))
        return;
    trail_eye_resume = ghost + 0x870;
    trail_other_resume = ghost + 0x87c;
    trail_reset_hook = hook_addr(reset, (uintptr_t)eye_trails_reset);
    hook_addr(update, (uintptr_t)eye_trails_update);
    hook_addr(draw, (uintptr_t)eye_trails_draw);
    hook_addr(ghost + 0x868, (uintptr_t)eye_trails_bridge);
}
