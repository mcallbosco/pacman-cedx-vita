#include "game_loading.h"
#include "game_patch.h"
#include "preloader.h"
#include <kubridge.h>
#include <stdatomic.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

extern void *dlsym_soloader(void *, const char *);
static void (*native_sincos)(float, float *, float *);
static uintptr_t file_stream_vtable;
static uintptr_t keyframe_resume __attribute__((used));
static uintptr_t resource_load_resume __attribute__((used));

/* Cache exact results from the existing math implementation. A contended
 * entry simply computes the answer; no loader thread waits on this cache. */
typedef struct {
    atomic_uint busy;
    unsigned valid, angle;
    float sine, cosine;
} AngleEntry;
static AngleEntry angles[1024];

static uint16_t read16(const unsigned char *p) {
    uint16_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static float read_fixed(const unsigned char *p) {
    int32_t value;
    memcpy(&value, p, sizeof(value));
    return (float)value / 10000.0f;
}

static void decode_polar(const unsigned char *p, float *x, float *y) {
    unsigned angle = read16(p + 2);
    AngleEntry *entry = &angles[(angle * 2654435761u) >> 22];
    unsigned expected = 0;
    int owned = atomic_compare_exchange_strong_explicit(&entry->busy,
        &expected, 1, memory_order_acquire, memory_order_relaxed);
    float sine, cosine;
    if (owned && entry->valid && entry->angle == angle) {
        sine = entry->sine;
        cosine = entry->cosine;
    } else {
        float radians = (float)angle * 0x1.921fb6p+1f;
        radians = radians / 180.0f;
        native_sincos(radians, &sine, &cosine);
        if (owned) {
            entry->sine = sine;
            entry->cosine = cosine;
            entry->angle = angle;
            entry->valid = 1;
        }
    }
    if (owned)
        atomic_store_explicit(&entry->busy, 0, memory_order_release);
    /* The original format decoder performs INTEGER division here. */
    float radius = (float)(read16(p) / 100u);
    *x = radius * cosine;
    *y = radius * sine;
}

static int __attribute__((used, noinline)) load_keyframe(void *self, void *stream) {
    uintptr_t *object = stream;
    if (object[0] != file_stream_vtable ||
        (((unsigned char *)stream)[24] & ((unsigned char *)stream)[25] & 1))
        return 0;
    /* Consume only complete records from our existing in-memory files.
     * Unknown streams and truncated records retain the native read behavior. */
    const unsigned char *p = preloader_slurp_take((FILE *)object[5], 20);
    if (!p)
        return 0;

    float values[8];
    values[0] = (float)read16(p) * 0x1.111112p-5f;
    values[1] = read_fixed(p + 2);
    decode_polar(p + 6, &values[2], &values[3]);
    values[4] = (float)read16(p + 10) * 0x1.111112p-5f;
    values[5] = read_fixed(p + 12);
    decode_polar(p + 16, &values[6], &values[7]);

    const float epsilon = 0x1.4f8b58p-17f;
    float delta = values[1] - values[5];
    if (delta < 0.0f)
        delta = -delta;
    unsigned char constant =
        values[2] * values[2] + values[3] * values[3] < epsilon &&
        delta < epsilon &&
        values[6] * values[6] + values[7] * values[7] < epsilon;
    values[2] = (values[2] + values[0]) * 3.0f;
    values[3] = (values[3] + values[1]) * 3.0f;
    values[6] = (values[6] + values[4]) * 3.0f;
    values[7] = (values[7] + values[5]) * 3.0f;
    ((unsigned char *)self)[4] = constant;
    memcpy((char *)self + 8, values, sizeof(values));
    return 1;
}

static void __attribute__((naked)) keyframe_bridge(void) {
    __asm__ volatile(
        "push {r0, r1, r4, lr}\n"
        "bl load_keyframe\n"
        "cmp r0, #0\n"
        "pop {r0, r1, r4, lr}\n"
        "beq 1f\n"
        "add r0, r0, #32\n"
        "bx lr\n"
        "1: push {r7, lr}\n"
        "mov r7, sp\n"
        "sub sp, sp, #56\n"
        "mov r2, r1\n"
        "ldr r12, =keyframe_resume\n"
        "ldr r12, [r12]\n"
        "bx r12\n");
}

#define RESOURCE_COUNT 463
#define PATH_SLOTS 1024
static const char *(*resource_path)(void *);
static void **indexed_resources;
static const char *paths[PATH_SLOTS];
static uint16_t path_ids[PATH_SLOTS];
static atomic_uint paths_busy;

static unsigned path_hash(const char *path) {
    uint32_t hash = 2166136261u;
    while (*path)
        hash = (hash ^ (unsigned char)*path++) * 16777619u;
    return hash & (PATH_SLOTS - 1);
}

static int find_resource(void *self, const char *path) {
    void **items = ((void ***)self)[1];
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&paths_busy, &expected, 1,
            memory_order_acquire, memory_order_relaxed)) {
        for (int i = 0; i < RESOURCE_COUNT; ++i)
            if (!strcmp(path, resource_path(items[i])))
                return i;
        return -1;
    }
    if (indexed_resources != items) {
        memset(paths, 0, sizeof(paths));
        for (unsigned i = 0; i < RESOURCE_COUNT; ++i) {
            const char *name = resource_path(items[i]);
            unsigned slot = path_hash(name);
            while (paths[slot] && strcmp(paths[slot], name))
                slot = (slot + 1) & (PATH_SLOTS - 1);
            /* The native scan returns the first ID for duplicate paths. */
            if (!paths[slot]) {
                paths[slot] = name;
                path_ids[slot] = i;
            }
        }
        indexed_resources = items;
    }
    unsigned slot = path_hash(path);
    while (paths[slot] && strcmp(paths[slot], path))
        slot = (slot + 1) & (PATH_SLOTS - 1);
    int result = paths[slot] ? path_ids[slot] : -1;
    atomic_store_explicit(&paths_busy, 0, memory_order_release);
    return result;
}

static void __attribute__((used, noinline)) reset_paths(void) {
    while (atomic_exchange_explicit(&paths_busy, 1, memory_order_acquire)) {}
    indexed_resources = NULL;
    atomic_store_explicit(&paths_busy, 0, memory_order_release);
}

static void __attribute__((naked)) resource_load_bridge(void) {
    __asm__ volatile(
        "push {r0, r1, r4, lr}\n"
        "bl reset_paths\n"
        "pop {r0, r1, r4, lr}\n"
        "push {r4, r6, r7, lr}\n"
        "add r7, sp, #8\n"
        "sub sp, sp, #112\n"
        "mov r2, r1\n"
        "ldr r12, =resource_load_resume\n"
        "ldr r12, [r12]\n"
        "bx r12\n");
}

/* Each native loop fills a contiguous 65-by-47 byte array. Keep the character
 * array's 0xff sentinel and the two extra byte resets alongside it. */
#define MAP_BUFFER_CELLS (65 * 47)

static void clear_map_flippers(void *map) {
    memset((char *)map + 0x6b80, 0, MAP_BUFFER_CELLS);
}

static void clear_map_smoothers(void *map) {
    memset((char *)map + 0x776f, 0, MAP_BUFFER_CELLS);
}

static void clear_map_characters(void *map) {
    ((unsigned char *)map)[24] = 0;
    memset((char *)map + 25, 0xff, MAP_BUFFER_CELLS);
    ((unsigned char *)map)[0x8f9c] = 0;
}

static void clear_map_pellet_animation(void *map) {
    memset((char *)map + 0x2fd5, 0, MAP_BUFFER_CELLS);
}

void game_loading_install_hooks(void) {
    static const struct {
        const char *symbol;
        size_t size;
        uint32_t hash;
        void (*replacement)(void *);
    } map_clears[] = {
        {"_ZN9newPacman10cMapBuffer17clearFliperBufferEv", 0x6a, 0x33f7c2bd, clear_map_flippers},
        {"_ZN9newPacman10cMapBuffer18clearSmooserBufferEv", 0x6a, 0x7893f47b, clear_map_smoothers},
        {"_ZN9newPacman10cMapBuffer20clearCharactorBufferEv", 0x72, 0x13aa61be, clear_map_characters},
        {"_ZN9newPacman10cMapBuffer18clearEsaAnimBufferEv", 0x6a, 0xe1c26b38, clear_map_pellet_animation}
    };
    for (size_t i = 0; i < sizeof(map_clears) / sizeof(map_clears[0]); ++i) {
        uintptr_t clear = game_patch_checked_function(
            map_clears[i].symbol, map_clears[i].size, map_clears[i].hash);
        if (clear)
            hook_addr(clear, (uintptr_t)map_clears[i].replacement);
    }

    uintptr_t keyframe = game_patch_checked_function(
        "_ZN3sys5runny13RunnyKeyframe8LoadFromEPNS_6StreamE", 0x1a0, 0x96072187);
    uintptr_t stream = game_patch_checked_function(
        "_ZN3sys10FileStream10ReadBufferEPvi", 0x84, 0xbdc91d54);
    uintptr_t vtable = so_symbol(&so_mod, "_ZTVN3sys10FileStreamE");
    native_sincos = dlsym_soloader(NULL, "sincosf");
    if (keyframe && stream && vtable && native_sincos) {
        file_stream_vtable = vtable + 8;
        keyframe_resume = keyframe + 8;
        hook_addr(keyframe, (uintptr_t)keyframe_bridge);
    }

    uintptr_t glyph = game_patch_checked_function(
        "_ZN3sys12FontTrueType9MakeGlyphEt", 0x1f0, 0xddbfa4a9);
    if (glyph) {
        /* Character lookup uses the charmap, not the current size. Keep
         * the later setup on the selected face, including fallback fonts. */
        const uint16_t nops[2] = {0xbf00, 0xbf00};
        kuKernelCpuUnrestrictedMemcpy((void *)((glyph & ~1u) + 0x46), nops, 4);
        kuKernelCpuUnrestrictedMemcpy((void *)((glyph & ~1u) + 0x5a), nops, 4);
    }

    uintptr_t lookup = game_patch_checked_function(
        "_ZN3sys10ResManager20GetResourceIdForPathEPKc", 0xb0, 0xfcbc0c55);
    uintptr_t load = game_patch_checked_function(
        "_ZN3sys10ResManager8LoadDataEPKc", 0x120, 0x46804adf);
    uintptr_t path = game_patch_checked_function(
        "_ZN3sys7ResItem7GetPathEv", 0x18, 0x5f8020c6);
    if (lookup && load && path) {
        resource_path = (void *)path;
        resource_load_resume = load + 8;
        hook_addr(load, (uintptr_t)resource_load_bridge);
        hook_addr(lookup, (uintptr_t)find_resource);
    }
}
