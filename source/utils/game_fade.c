#include "game_fade.h"
#include "game_patch.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static void (*fade_free)(void *);
static void **effect_root;
static uintptr_t effect_scan_resume __attribute__((used));
static void *(*reset_allocate)(uint32_t, void *);
static void (*reset_construct)(void *, void *, uint32_t, uint32_t, int32_t);
static uintptr_t reset_parent_vtable;
static uintptr_t reset_vtable[8];
static uintptr_t reset_fallback __attribute__((used));
static uintptr_t reset_done __attribute__((used));

static uint32_t word(const void *p, unsigned offset) {
    uint32_t value;
    memcpy(&value, (const char *)p + offset, 4);
    return value;
}

static void *pointer(const void *p, unsigned offset) {
    return (void *)(uintptr_t)word(p, offset);
}

static uint32_t children_count(const void *task) {
    return word(task, 16);
}

static void *grid_point(const void *sprite, uint32_t x, uint32_t y) {
    /* Native address arithmetic wraps in 32 bits, without bounds checks. */
    uint32_t index = x + y * (word(sprite, 32) + 1);
    return (void *)(uintptr_t)(word(sprite, 28) + index * 52);
}

static uint32_t grid_width(const void *sprite) {
    return word(sprite, 32) + 1;
}

static uint32_t grid_height(const void *sprite) {
    return word(sprite, 36) + 1;
}

static void fade_column(void *task) {
    int32_t delay = (int32_t)word(task, 44);
    if (delay > 0) {
        --delay;
        memcpy((char *)task + 44, &delay, 4);
        return;
    }
    int32_t rows = (int32_t)word(task, 36);
    uint32_t finished = 0;
    for (int32_t row = 0; row < rows; ++row) {
        void *point = grid_point(pointer(task, 28), word(task, 48), (uint32_t)row);
        float alpha, step;
        memcpy(&alpha, (char *)point + 48, 4);
        memcpy(&step, (char *)task + 40, 4);
        __asm__("vadd.f32 %0, %0, %1" : "+t" (alpha) : "t" (step));
        if (step > 0.0f) {
            if (alpha >= 1.0f) {
                alpha = 1.0f;
                ++finished;
            }
        } else if (alpha <= 0.0f) {
            alpha = 0.0f;
            ++finished;
        }
        memcpy((char *)point + 48, &alpha, 4);
    }
    if ((int32_t)finished >= rows)
        fade_free(task);
}

/* One scheduled child replaces the 27 simultaneous endpoint columns. Keeping
 * a child is deliberate: Execute can run the parent while skipping children.
 * A bit remains set until both points in that column have reached opacity,
 * just as the native children retire independently for unusual alpha values. */
static void reset_columns(void *task) {
    uint32_t pending = word(task, 48);
    for (uint32_t column = 0; column < 27; ++column) {
        uint32_t bit = 1u << column;
        if (!(pending & bit))
            continue;
        unsigned finished = 0;
        for (uint32_t row = 0; row < 2; ++row) {
            void *point = grid_point(pointer(task, 28), column, row);
            float alpha;
            memcpy(&alpha, (char *)point + 48, 4);
            float step = 1.0f;
            __asm__("vadd.f32 %0, %0, %1" : "+t" (alpha) : "t" (step));
            if (alpha >= 1.0f) {
                alpha = 1.0f;
                ++finished;
            }
            memcpy((char *)point + 48, &alpha, 4);
        }
        if (finished == 2)
            pending &= ~bit;
    }
    memcpy((char *)task + 48, &pending, 4);
    if (!pending)
        fade_free(task);
}

static int __attribute__((used, noinline)) create_reset_child(void *task) {
    void *sprite = pointer(task, 28);
    const void *end = (const char *)task + 8;
    if (word(task, 0) != reset_parent_vtable || word(task, 32) != 0 ||
        word(task, 36) != 27 || word(task, 40) != 0x3f800000u ||
        word(task, 44) != 0 || word(task, 48) != 0 ||
        word(task, 16) != 0 || pointer(task, 8) != end ||
        pointer(task, 12) != end || !sprite || !pointer(sprite, 28) ||
        word(sprite, 32) != 26 || word(sprite, 36) != 1)
        return 0;

    void *child = reset_allocate(52, task);
    reset_construct(child, sprite, 0, 0x3f800000u, 0);
    uint32_t pending = (1u << 27) - 1;
    uintptr_t vtable = (uintptr_t)&reset_vtable[2];
    memcpy(child, &vtable, 4);
    memcpy((char *)child + 48, &pending, 4);
    return 1;
}

static void __attribute__((naked)) reset_create_bridge(void) {
    __asm__ volatile(
        "ldr r0, [sp, #20]\n"
        "push {r4, lr}\n"
        "bl create_reset_child\n"
        "pop {r4, lr}\n"
        "cmp r0, #0\n"
        "bne 1f\n"
        /* Replay the displaced native type-0 loop initialization. */
        "movs r0, #0\n"
        "str r0, [sp, #28]\n"
        "ldr r0, [sp, #28]\n"
        "ldr ip, =reset_fallback\n"
        "ldr ip, [ip]\n"
        "bx ip\n"
        "1:\n"
        "ldr ip, =reset_done\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

static void install_reset_child(uintptr_t alpha, uintptr_t column) {
    uintptr_t constructor = game_patch_checked_code(
        alpha + 0x202, "map alpha column constructor", 0x74, 0xac6aefc3u);
    uintptr_t destructors = game_patch_checked_code(
        alpha + 0x2c0, "map alpha column destructors", 0x38, 0xfce22653u);
    uintptr_t allocate = game_patch_checked_function(
        "_ZN3sys5cTasknwEjPS0_", 0x34, 0x5e4341cfu);
    uintptr_t parent = game_patch_checked_function(
        "_ZN9newPacman22cTsTaskEffectAlphaCtrlC2EPN3sys7cSpriteENS0_8CtrlTypeEfi",
        0x70, 0x33245140u);
    uintptr_t parent_vtable = so_symbol(&so_mod,
        "_ZTVN9newPacman22cTsTaskEffectAlphaCtrlE");
    if (!constructor || !destructors || !allocate || !parent || !parent_vtable ||
        !game_patch_checked_function("_ZN3sys5cTaskC2Ev", 0x38, 0x53221002u) ||
        !game_patch_checked_function("_ZN3sys5cTask7SetFuncEv", 0x16, 0x8d5a97c9u))
        return;

    /* The checked hidden constructor loads this PC-relative, six-slot table.
     * Retain its RTTI, both destructors, refresh and draw/exec callbacks. */
    const void *table = (void *)((constructor & ~(uintptr_t)1) + 0x3a +
                                word((void *)(constructor & ~(uintptr_t)1), 0x70));
    const uintptr_t expected[] = {
        0, 0, destructors, destructors + 0x1a, column,
        so_symbol(&so_mod, "_ZN3sys5cTask7RefreshEv"),
        so_symbol(&so_mod, "_ZN3sys5cTask4ExecEv"),
        so_symbol(&so_mod, "_ZN3sys5cTask4DrawEv")
    };
    for (unsigned i = 0; i < 8; ++i)
        if (word(table, i * 4) != expected[i] || (i > 1 && !expected[i]))
            return;
    memcpy(reset_vtable, table, sizeof(reset_vtable));
    reset_vtable[4] = (uintptr_t)reset_columns;
    reset_parent_vtable = parent_vtable + 8;
    reset_allocate = (void *)allocate;
    reset_construct = (void *)constructor;
    reset_fallback = alpha + 0x5e;
    reset_done = alpha + 0x1de;
    hook_addr(alpha + 0x56, (uintptr_t)reset_create_bridge);
}

static void __attribute__((used, noinline)) scan_map_effects(void *layer) {
    uint32_t id = word(layer, 264);
    if (id > 1)
        return;
    const void *root = *effect_root;
    const void *end = (const char *)root + 8;
    for (const void *node = pointer(root, 12); node != end; node = pointer(node, 4)) {
        const void *effect = pointer(node, 8);
        if (word(effect, 88) == id + 6)
            ((unsigned char *)layer)[224] = 1;
    }
}

static void __attribute__((naked)) map_effect_bridge(void) {
    __asm__ volatile(
        "ldr r0, [sp, #104]\n"
        "push {r4, lr}\n"
        "bl scan_map_effects\n"
        "pop {r4, lr}\n"
        "ldr ip, =effect_scan_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_fade_install_hooks(void) {
    uintptr_t grid = game_patch_checked_function(
        "_ZN3sys7cSprite12GetGridPointEjj", 0x38, 0xb6c115cfu);
    uintptr_t width = game_patch_checked_function(
        "_ZN3sys7cSprite17GetGridPointWidthEv", 0x12, 0xc34fd6d3u);
    uintptr_t height = game_patch_checked_function(
        "_ZN3sys7cSprite18GetGridPointHeightEv", 0x12, 0xe049ce13u);
    uintptr_t alpha = game_patch_checked_function(
        "_ZN9newPacman22cTsTaskEffectAlphaCtrl4FuncEv", 0x202, 0xa63b4667u);
    fade_free = (void *)game_patch_checked_function(
        "_ZN3sys5cTask4FreeEv", 0x1e, 0x832c9939u);
    if (grid && width && height && alpha && fade_free) {
        uintptr_t column = game_patch_checked_code(
            alpha + 0x2f8, "map alpha column", 0xdc, 0x9577d03au);
        if (column) {
            install_reset_child(alpha, column);
            hook_addr(column, (uintptr_t)fade_column);
        }
    }
    if (grid) hook_addr(grid, (uintptr_t)grid_point);
    if (width) hook_addr(width, (uintptr_t)grid_width);
    if (height) hook_addr(height, (uintptr_t)grid_height);

    uintptr_t count = game_patch_checked_function(
        "_ZN3sys5cTask14GetChildrenCntEv", 0x38, 0xd478c14bu);
    /* Its libc++ empty/size helper chain only reads the stored count. */
    if (count &&
        game_patch_checked_code(count + 0x3d0, "task list queries", 0x2e, 0x1361fa34u) &&
        game_patch_checked_code(count - 0x8584a, "task list empty", 0x1e, 0x6e67d5bbu) &&
        game_patch_checked_code(count - 0x85744, "task list count pointer", 0x18, 0xb59ede72u) &&
        game_patch_checked_code(count - 0x8572c, "task list count storage", 0x16, 0x1363a2bfu) &&
        game_patch_checked_code(count - 0xec0c6, "task list count reference", 0xe, 0x6f470d90u))
        hook_addr(count, (uintptr_t)children_count);

    uintptr_t map = game_patch_checked_function(
        "_ZN3sys14TextureManager18GetCurCharSkinInfoEv", 0x1c, 0x5d7be0a0u);
    effect_root = (void **)so_symbol(&so_mod,
        "_ZN9newPacman27cOnCharactorKindBundlerTask20pEffectEvtVectorRootE");
    uintptr_t layer = map ? game_patch_checked_code(
        map + 0x3c8, "map layer update", 0x5a0, 0xd98ccb70u) : 0;
    if (layer && effect_root) {
        effect_scan_resume = layer + 0x2dc;
        hook_addr(layer + 0x256, (uintptr_t)map_effect_bridge);
    }
}
