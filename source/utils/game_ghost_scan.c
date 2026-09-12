#include "game_ghost_scan.h"
#include "game_patch.h"
#include "game_spacing.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math")

/* These are acceleration limits, not limits on the game's ghost population. */
#define SCAN_LIMIT 2048
#define TRAIN_SLOTS 4096
#define CELL_BUCKETS 1024

static uintptr_t ghost_func;
static uintptr_t ghost_draw, empty_exec;
static void *active_ghost;
static unsigned callback_depth;
static uintptr_t train_mutation_resume __attribute__((used));
static uintptr_t mode_mutation_resume __attribute__((used));
static so_hook train_reset_hook;
static void **indexed_train_begin, **indexed_train_end;
static uint16_t train_slots[TRAIN_SLOTS];

typedef struct {
    void *ghost;
    int x, y;
    uint16_t next;
} SpacingEntry;

static SpacingEntry spacing_entries[SCAN_LIMIT];
static uint16_t cell_heads[CELL_BUCKETS];
static void *spacing_parent;
static uint32_t spacing_first, spacing_count;
static unsigned active_bucket;
static int active_x, active_y;

static uint32_t word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static void *pointer(const void *object, size_t offset) {
    return (void *)(uintptr_t)word(object, offset);
}

void game_ghost_scan_reset(void) {
    indexed_train_begin = NULL;
    spacing_parent = NULL;
}

/* Stop using either index for the remainder of a mutating callback, including
 * queries made inside the mutation itself. Native objects remain untouched. */
static void __attribute__((used, noinline)) suspend_scans(void) {
    active_ghost = NULL;
    game_ghost_scan_reset();
}

static int scan_active(void) {
    if (!active_ghost)
        return 0;
    if (((const unsigned char *)active_ghost)[0xed] != 0) {
        suspend_scans();
        return 0;
    }
    return 1;
}

static unsigned train_hash(void *ghost) {
    uint32_t bits = (uintptr_t)ghost;
    return ((bits >> 2) * 2654435761u) >> 20;
}

int game_ghost_scan_train(void ***list, void *ghost, void ***entry) {
    if (!scan_active())
        return 0;
    void **begin = list[0], **end = list[1];
    if (begin == end || end - begin < 64 || end - begin > SCAN_LIMIT)
        return 0;
    if (indexed_train_begin != begin || indexed_train_end != end) {
        memset(train_slots, 0, sizeof(train_slots));
        for (unsigned i = 0; i < (unsigned)(end - begin); ++i) {
            unsigned slot = train_hash(begin[i]);
            while (train_slots[slot] && begin[train_slots[slot] - 1] != begin[i])
                slot = (slot + 1) & (TRAIN_SLOTS - 1);
            /* Duplicate pointers retain the first entry's position. */
            if (!train_slots[slot])
                train_slots[slot] = i + 1;
        }
        indexed_train_begin = begin;
        indexed_train_end = end;
    }
    unsigned slot = train_hash(ghost);
    while (train_slots[slot]) {
        void **found = begin + train_slots[slot] - 1;
        if (*found == ghost) {
            *entry = found;
            return 1;
        }
        slot = (slot + 1) & (TRAIN_SLOTS - 1);
    }
    *entry = NULL;
    return 1;
}

static int cell(const void *ghost, int *x, int *y) {
    float px, py;
    memcpy(&px, (const char *)ghost + 0xc4, sizeof(px));
    memcpy(&py, (const char *)ghost + 0xc8, sizeof(py));
    /* Reject non-finite and extreme inputs before conversion. Falling back
     * preserves the native comparisons for all float bit patterns. */
    if (!(px > -65536.0f && px < 65536.0f && py > -65536.0f && py < 65536.0f))
        return 0;
    px *= 0.5f;
    py *= 0.5f;
    *x = (int)px;
    *y = (int)py;
    *x -= px < (float)*x;
    *y -= py < (float)*y;
    return 1;
}

static unsigned cell_hash(int x, int y) {
    return ((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u) & (CELL_BUCKETS - 1);
}

static int build_spacing(void *parent) {
    spacing_parent = NULL;
    uint32_t count = word(parent, 16);
    if (count < 16 || count > SCAN_LIMIT)
        return 0;
    memset(cell_heads, 0, sizeof(cell_heads));
    unsigned i = 0;
    void *end = (char *)parent + 8;
    for (void *node = pointer(parent, 12); node != end; node = pointer(node, 4)) {
        if (i == count)
            return 0;
        SpacingEntry *entry = &spacing_entries[i];
        entry->ghost = pointer(node, 8);
        if (!cell(entry->ghost, &entry->x, &entry->y))
            return 0;
        unsigned bucket = cell_hash(entry->x, entry->y);
        ++i;
        if (entry->ghost == active_ghost) {
            active_bucket = bucket;
            active_x = entry->x;
            active_y = entry->y;
        }
    }
    if (i != count)
        return 0;
    /* Keep each bucket in original sibling order, including duplicates. */
    while (i) {
        SpacingEntry *entry = &spacing_entries[--i];
        unsigned bucket = cell_hash(entry->x, entry->y);
        entry->next = cell_heads[bucket];
        cell_heads[bucket] = i + 1;
    }
    spacing_first = word(parent, 12);
    spacing_count = count;
    spacing_parent = parent;
    return 1;
}

int game_ghost_scan_spacing(void *ghost, int *near_train) {
    if (!scan_active() || ghost != active_ghost)
        return -1;
    void *parent = pointer(ghost, 4);
    /* Dense overlapping groups often stop at the first other sibling. Keep
     * that inexpensive case ahead of any spatial-index setup or maintenance. */
    void *node = pointer(parent, 12), *end = (char *)parent + 8;
    for (unsigned i = 0; i < 2 && node != end; ++i, node = pointer(node, 4)) {
        void *other = pointer(node, 8);
        if (other != ghost) {
            int rule = game_spacing_rule(ghost, other, near_train);
            if (rule)
                return rule;
        }
    }
    int x, y;
    if (!cell(ghost, &x, &y))
        return -1;
    if (spacing_parent != parent || spacing_first != word(parent, 12) ||
        spacing_count != word(parent, 16)) {
        if (!build_spacing(parent))
            return -1;
    }
    /* The native predicate requires an exactly equal row or column and a
     * distance strictly below two. Only these five two-unit cells can match. */
    static const int8_t offsets[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    unsigned first_match = SCAN_LIMIT;
    int rule = 0;
    for (unsigned c = 0; c < 5; ++c) {
        int cx = x + offsets[c][0], cy = y + offsets[c][1];
        for (unsigned link = cell_heads[cell_hash(cx, cy)]; link;
             link = spacing_entries[link - 1].next) {
            unsigned rank = link - 1;
            SpacingEntry *entry = &spacing_entries[rank];
            if (rank >= first_match)
                break;
            if (entry->x != cx || entry->y != cy || entry->ghost == ghost)
                continue;
            int selected = game_spacing_rule(ghost, entry->ghost, near_train);
            if (selected) {
                rule = selected;
                first_match = rank;
            }
        }
    }
    return rule;
}

static void update_spacing(void *ghost) {
    if (!spacing_parent)
        return;
    int x, y;
    if (pointer(ghost, 4) != spacing_parent || !cell(ghost, &x, &y)) {
        spacing_parent = NULL;
        return;
    }
    if (x == active_x && y == active_y)
        return;
    unsigned bucket = cell_hash(x, y);
    uint16_t *link = &cell_heads[active_bucket];
    while (*link) {
        SpacingEntry *entry = &spacing_entries[*link - 1];
        if (entry->ghost == ghost) {
            entry->x = x;
            entry->y = y;
            if (bucket != active_bucket) {
                unsigned moved = *link;
                *link = entry->next;
                uint16_t *dest = &cell_heads[bucket];
                while (*dest && *dest < moved)
                    dest = &spacing_entries[*dest - 1].next;
                entry->next = *dest;
                *dest = moved;
                continue;
            }
        }
        link = &entry->next;
    }
}

void game_ghost_scan_call(void *task, void (*callback)(void *)) {
    /* LoopFunc executes update, Exec and Draw together for each task. These
     * checked render/no-op callbacks do not change ghost positions or lists. */
    if ((uintptr_t)callback == ghost_draw || (uintptr_t)callback == empty_exec) {
        ++callback_depth;
        callback(task);
        --callback_depth;
        return;
    }
    /* Task execution is serial. Only the checked normal ghost Func changes
     * one indexed position; other callbacks and reentrant execution invalidate
     * both indexes. Flags, directions and spacing parameters stay live. */
    void *saved = active_ghost;
    if (!callback_depth && (uintptr_t)callback == ghost_func &&
        ((const unsigned char *)task)[0xed] == 0) {
        active_ghost = task;
        if (spacing_parent) {
            int x, y;
            if (pointer(task, 4) != spacing_parent || !cell(task, &x, &y))
                spacing_parent = NULL;
            else {
                active_bucket = cell_hash(x, y);
                active_x = x;
                active_y = y;
            }
        }
    } else {
        suspend_scans();
    }
    ++callback_depth;
    callback(task);
    --callback_depth;
    if (active_ghost == task && scan_active())
        update_spacing(task);
    else
        game_ghost_scan_reset();
    active_ghost = saved;
}

/* tryEnterTrain +0x118 is reached only after the freeze and trainIsIjike
 * guards, before any vector reordering or other-ghost position writes. Replay
 * the replaced eight bytes, preserving all live registers and the native frame. */
static void __attribute__((naked)) train_mutation_bridge(void) {
    __asm__ volatile(
        "push {r0-r3, ip, lr}\n"
        "bl suspend_scans\n"
        "pop {r0-r3, ip, lr}\n"
        "movs r0, #0\n"
        "strb r0, [r7, #-81]\n"
        "movs r0, #0\n"
        "push {r0, r1}\n"
        "ldr r0, =train_mutation_resume\n"
        "ldr r0, [r0]\n"
        "str r0, [sp, #4]\n"
        "pop {r0, pc}\n");
}

static void __attribute__((naked)) mode_mutation_bridge(void) {
    __asm__ volatile(
        "push {r0-r3, ip, lr}\n"
        "bl suspend_scans\n"
        "pop {r0-r3, ip, lr}\n"
        "push {r4, r6, r7, lr}\n"
        "add r7, sp, #8\n"
        "sub sp, #64\n"
        "mov r2, r1\n"
        "push {r0, r1}\n"
        "ldr r0, =mode_mutation_resume\n"
        "ldr r0, [r0]\n"
        "str r0, [sp, #4]\n"
        "pop {r0, pc}\n");
}

static void reset_train(void) {
    suspend_scans();
    so_hook_unpatch(&train_reset_hook);
    ((void (*)(void))train_reset_hook.thumb_addr)();
    so_hook_repatch(&train_reset_hook);
}

void game_ghost_scan_install_hooks(void) {
    /* Install before the existing trail patch changes OnModeNormal. The
     * callback boundary is supplied by the checked task Execute replacement. */
    uintptr_t func = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask4FuncEv", 0x934, 0x15535a5eu);
    uintptr_t enter = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask13tryEnterTrainEv", 0x76c, 0xeab19214u);
    uintptr_t mode = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask12SetModeForceEh", 0x110, 0x2c0f4f61u);
    uintptr_t reset = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask15resetGhostTrainEv", 0x28, 0x6a376592u);
    uintptr_t draw = game_patch_checked_function(
        "_ZN9newPacman12cOnGhostTask4DrawEv", 0x40, 0x5877c91du);
    uintptr_t exec = game_patch_checked_function(
        "_ZN3sys5cTask4ExecEv", 0xc, 0x87cf7ea5u);
    if (!func || !enter || !mode || !reset || !draw || !exec ||
        !game_patch_checked_function("_ZN9newPacman12cOnGhostTask12OnModeNormalEv", 0x230, 0xbdb30a78u) ||
        !game_patch_checked_function("_ZN9newPacman12cOnGhostTask3AimEfff", 0xbf0, 0xdbefc125u) ||
        !game_patch_checked_function("_ZN9newPacman12cOnGhostTask12changeTargetEv", 0x244, 0x4f8792dau))
        return;
    train_mutation_resume = enter + 0x120;
    mode_mutation_resume = mode + 8;
    hook_addr(enter + 0x118, (uintptr_t)train_mutation_bridge);
    hook_addr(mode, (uintptr_t)mode_mutation_bridge);
    train_reset_hook = hook_addr(reset, (uintptr_t)reset_train);
    ghost_func = func;
    ghost_draw = draw;
    empty_exec = exec;
}
