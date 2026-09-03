#include "game_ghost_particles.h"
#include "game_patch.h"
#include "settings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

extern float particle_sine(float) __asm__("sinf") __attribute__((pcs("aapcs-vfp")));
extern float particle_cosine(float) __asm__("cosf") __attribute__((pcs("aapcs-vfp")));

enum { SWEAT_LIMIT = 16, SPARK_LIMIT = 30, PARTICLE_BYTES = 248 };
static void *particles[2][SPARK_LIMIT];
static unsigned burst_budget = 12, split_budget = 6;
static uint32_t particle_random = 0x504143u;
static void **particle_root;
static int (*particle_is_game)(void), (*particle_is_preview)(void), (*particle_is_paused)(void);
static void *(*particle_add_child)(void *, void *);
static void *(*sweat_ctor)(void *, float, float, float, float, float, float);
static void *(*spark_ctor)(void *, float, float, float, float, float, float, int);
static so_hook sweat_destroy_hook, spark_destroy_hook;
static uintptr_t ghost_particle_resume __attribute__((used));
static uintptr_t spark_split_resume __attribute__((used));

static float number(const void *object, unsigned offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static unsigned word(const void *object, unsigned offset) {
    unsigned value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

void game_ghost_particles_next_frame(void) {
    burst_budget = 12;
    split_budget = 6;
}

static int particle_scene(void) {
    return setting_ghostEatParticles && particle_root && *particle_root &&
        particle_is_game() && !particle_is_preview() && !particle_is_paused();
}

static int spawn_particle(int gold, int child, float x, float y, float vx, float vy) {
    unsigned *budget = child ? &split_budget : &burst_budget;
    if (!*budget || !isfinite(x) || !isfinite(y) || !isfinite(vx) || !isfinite(vy) ||
        fabsf(x) > 2048.0f || fabsf(y) > 2048.0f || fabsf(vx) > 4.0f || fabsf(vy) > 4.0f)
        return 0;
    unsigned limit = gold ? SPARK_LIMIT : SWEAT_LIMIT;
    unsigned slot = 0;
    while (slot < limit && particles[gold][slot]) ++slot;
    if (slot == limit)
        return 0;
    /* Match native cTask::operator new(size, parent): zero the object and
     * attach it before construction. Android free uses this same allocator. */
    void *task = calloc(1, PARTICLE_BYTES);
    if (!task)
        return 0;
    particles[gold][slot] = task;
    --*budget;
    particle_add_child(*particle_root, task);
    if (gold)
        spark_ctor(task, x, y, vx, vy, 0.0f, 0.5f, !child);
    else
        sweat_ctor(task, x, y, vx, vy, 0.0f, 0.5f);
    return 1;
}

static void *particle_destroy(void *task, int gold, so_hook *hook) {
    for (unsigned i = 0; i < SPARK_LIMIT; ++i)
        if (particles[gold][i] == task) {
            particles[gold][i] = NULL;
            break;
        }
    /* Release slots on destruction, including course restart/root cleanup.
     * Retained addresses are only compared, never dereferenced after free. */
    so_hook_unpatch(hook);
    void *result = ((void *(*)(void *))hook->thumb_addr)(task);
    so_hook_repatch(hook);
    return result;
}

static void *destroy_sweat(void *task) { return particle_destroy(task, 0, &sweat_destroy_hook); }
static void *destroy_spark(void *task) { return particle_destroy(task, 1, &spark_destroy_hook); }

static void __attribute__((used, noinline)) ghost_particles_emit(void *pacman, void *ghost) {
    if (!particle_scene())
        return;
    float x = (number(pacman, 0xc4) + number(ghost, 0xc4)) * 0.5f;
    float y = (number(pacman, 0xc8) + number(ghost, 0xc8)) * 0.5f;
    int16_t direction;
    memcpy(&direction, (const char *)pacman + 0x18c, sizeof(direction));
    unsigned combo = word(pacman, 0x1dc);
    if (direction < 0 || direction > 3 || !combo || combo > 8)
        return;
    const float pi = 3.141592741f;
    float angle = ((4 - direction) & 3) * (pi * 0.5f) + pi * 0.5f;
    if (combo <= 2) {
        for (unsigned i = 0; i < 8; ++i) {
            /* Independent visual RNG preserves gameplay randomness. PC uses
             * alternating side bursts with a narrow cubic angle spread. */
            particle_random = (particle_random + 1) * 0x350bu;
            float spread = (particle_random & 0x7fffu) / 32767.0f * 1.32f - 0.66f;
            float a = angle + (i & 1 ? pi : 0.0f) + spread * spread * spread * (pi * 0.5f);
            float vx = particle_cosine(a) * 0.2f, vy = particle_sine(a) * 0.2f;
            if (!spawn_particle(0, 0, x + vx, y + vy, vx, vy))
                break;
        }
    } else {
        /* Same two six-spark fans as PC, interleaved so a capped burst
         * still covers both sides of the collision. */
        for (unsigned i = 0; i < 12; ++i) {
            unsigned step = i / 2;
            float spread = step * (2.0f / 5.0f) - 1.0f;
            float a = angle + (i & 1 ? -pi / 18.0f : pi / 18.0f) + step * pi;
            float velocity = a + spread * (pi / 12.0f);
            if (!spawn_particle(1, 0,
                    x - spread * 0.33f * particle_cosine(a),
                    y - spread * 0.33f * particle_sine(a),
                    0.33f * particle_cosine(velocity), 0.33f * particle_sine(velocity)))
                break;
        }
    }
}

static void __attribute__((used, noinline)) ghost_particle_split(void *spark, float vx, float vy) {
    if (particle_scene())
        spawn_particle(1, 1, number(spark, 0xc4), number(spark, 0xc8), vx, vy);
}

/* The native ghost-eat event has been completed at this point. Preserve live
 * registers and replay the mode comparison before resuming gameplay. */
static void __attribute__((naked)) ghost_particles_bridge(void) {
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n"
        "vpush {s0-s15}\n"
        "ldr r0, [sp, #140]\n"
        "ldr r1, [sp, #172]\n"
        "bl ghost_particles_emit\n"
        "vpop {s0-s15}\n"
        "pop {r0-r5, ip, lr}\n"
        "ldr r0, [sp, #76]\n"
        "ldrb r0, [r0, #236]\n"
        "cmp r0, #4\n"
        "ldr ip, =ghost_particle_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

/* Replace only the native splitting allocation/constructor block. The native
 * wall test, bounce behavior, movement, draw and lifetime remain in place. */
static void __attribute__((naked)) ghost_particle_split_bridge(void) {
    __asm__ volatile(
        "push {r0-r5, ip, lr}\n"
        "vpush {s0-s15}\n"
        "ldr r0, [sp, #160]\n"
        "ldr r1, [sp, #180]\n"
        "ldr r2, [sp, #176]\n"
        "bl ghost_particle_split\n"
        "vpop {s0-s15}\n"
        "pop {r0-r5, ip, lr}\n"
        "ldr ip, =spark_split_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_ghost_particles_install_hooks(void) {
    if (!setting_ghostEatParticles)
        return;
    uintptr_t eat = game_patch_checked_function(
        "_ZN9newPacman13cOnPacmanTask18TestPacmanEatGhostEv", 0x734, 0x05ace6f0u);
    uintptr_t spark = game_patch_checked_function(
        "_ZN9newPacman13cOnGhostSpark4FuncEv", 0x464, 0x33b2a95cu);
    uintptr_t sweat_destroy = game_patch_checked_function(
        "_ZN9newPacman13cOnGhostSweatD2Ev", 0x1a, 0x0ea83df8u);
    uintptr_t spark_destroy = game_patch_checked_function(
        "_ZN9newPacman13cOnGhostSparkD2Ev", 0xb8, 0x9844b6bdu);
    sweat_ctor = (void *)game_patch_checked_function(
        "_ZN9newPacman13cOnGhostSweatC2Effffff", 0x130, 0x22728fe7u);
    spark_ctor = (void *)game_patch_checked_function(
        "_ZN9newPacman13cOnGhostSparkC2Effffffb", 0x204, 0xe42543b8u);
    particle_add_child = (void *)game_patch_checked_function(
        "_ZN3sys5cTask8AddChildEPS0_", 0x5c, 0xc12a2dffu);
    particle_is_game = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence6IsGameEv", 0x14, 0x476f8a74u);
    particle_is_preview = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence9IsPreviewEv", 0x14, 0x1128e4ffu);
    particle_is_paused = (void *)game_patch_checked_function(
        "_ZN9newPacman12GetPauseFlagEv", 0x10, 0xd3471ef9u);
    void **root = (void *)so_symbol(&so_mod,
        "_ZN9newPacman27cOnCharactorKindBundlerTask11pEffectRootE");
    if (!eat || !spark || !sweat_destroy || !spark_destroy || !sweat_ctor || !spark_ctor ||
        !particle_add_child || !particle_is_game || !particle_is_preview || !particle_is_paused || !root ||
        !game_patch_checked_function("_ZN9newPacman13cOnPacmanTask4FuncEv", 0xef0, 0x00ae5623u) ||
        !game_patch_checked_function("_ZN9newPacman13cOnGhostSweat4FuncEv", 0x234, 0x56ca1f6fu) ||
        !game_patch_checked_function("_ZN9newPacman13cOnGhostSweatD0Ev", 0x1e, 0x62fbfb08u) ||
        !game_patch_checked_function("_ZN9newPacman13cOnGhostSparkD0Ev", 0x22, 0x909871a9u))
        return;
    particle_root = root;
    ghost_particle_resume = eat + 0x5a8;
    spark_split_resume = spark + 0x2a8;
    sweat_destroy_hook = hook_addr(sweat_destroy, (uintptr_t)destroy_sweat);
    spark_destroy_hook = hook_addr(spark_destroy, (uintptr_t)destroy_spark);
    hook_addr(spark + 0x266, (uintptr_t)ghost_particle_split_bridge);
    hook_addr(eat + 0x5a0, (uintptr_t)ghost_particles_bridge);
}
