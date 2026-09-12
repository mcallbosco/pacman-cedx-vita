#include "game_primitive.h"
#include "game_patch.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

typedef struct Primitive Primitive;
struct __attribute__((may_alias)) Primitive {
    void (**vtable)(Primitive *);
    const char *name;
    Primitive *next, *prev;
    float depth;
};

typedef struct __attribute__((may_alias)) {
    float maximum;
    uint32_t buckets;
    float step;
    Primitive *first;
    uint32_t count;
} PrimitiveList;

static const char *primitive_sentinel_name;

static void primitive_add(PrimitiveList *list, Primitive *primitive) {
    if (!(primitive->depth < list->maximum) || primitive->depth < 0.0f)
        return;
    float depth = primitive->depth, step = list->step, bucket_float;
    uint32_t bucket;
    /* Keep native division and saturating ARM conversion for every float,
     * including rounded bucket boundaries and unusual step values. */
    __asm__("vdiv.f32 %1, %2, %3\n"
            "vcvt.u32.f32 %1, %1\n"
            "vmov %0, %1"
            : "=r" (bucket), "=&t" (bucket_float)
            : "t" (depth), "t" (step));
    if (bucket >= list->buckets)
        return;
    Primitive *node = list->first[bucket].next;
    while (node->depth < primitive->depth)
        node = node->next;
    /* Insert before equal depth entries: reverse execution then preserves
     * their submission order, including the original blend ordering. */
    node->prev->next = primitive;
    primitive->prev = node->prev;
    primitive->next = node;
    node->prev = primitive;
    ++list->count;
}

static void primitive_reset(PrimitiveList *list) {
    for (uint32_t i = 0; i <= list->buckets; ++i) {
        Primitive *node = list->first + i;
        node->name = primitive_sentinel_name;
        node->prev = (Primitive *)((uintptr_t)node - sizeof(*node));
        node->next = (Primitive *)((uintptr_t)node + sizeof(*node));
        float depth, step = list->step, index;
        __asm__("vmov %1, %2\n"
                "vcvt.f32.u32 %1, %1\n"
                "vmul.f32 %0, %3, %1"
                : "=t" (depth), "=&t" (index)
                : "r" (i), "t" (step));
        node->depth = depth;
    }
    list->first->prev = NULL;
    list->first[list->buckets].next = NULL;
    list->count = 0;
}

static void primitive_reverse(PrimitiveList *list) {
    Primitive *node = list->first + list->buckets;
    do {
        node = node->prev;
        node->vtable[0](node);
        /* Native sentinel draws still run. A draw can change its previous
         * link or the list's first pointer; read each after the callback. */
    } while (node != list->first);
}

void game_primitive_install_hooks(void) {
    if (!game_patch_checked_function(
            "_ZN3sys14cPrimitiveListC2EPNS_15cPrimitiveParamE", 0x10c, 0xcb7f1435u) ||
        !game_patch_checked_function(
            "_ZN3sys10cPrimitiveC2Ev", 0x28, 0xa84b9e4fu))
        return;
    uintptr_t add = game_patch_checked_function(
        "_ZN3sys14cPrimitiveList3AddEPNS_10cPrimitiveE", 0xc0, 0x6f3ad386u);
    uintptr_t reset = game_patch_checked_function(
        "_ZN3sys14cPrimitiveList5ResetEv", 0xa0, 0xd6318078u);
    uintptr_t reverse = game_patch_checked_function(
        "_ZN3sys14cPrimitiveList19ExecuteOrderReverseEv", 0x40, 0xae8993a8u);
    if (add)
        hook_addr(add, (uintptr_t)primitive_add);
    if (reset) {
        uint32_t name_offset;
        uintptr_t code = reset & ~(uintptr_t)1;
        memcpy(&name_offset, (const void *)(code + 0x9c), sizeof(name_offset));
        primitive_sentinel_name = (const char *)(code + 0x32 + name_offset);
        hook_addr(reset, (uintptr_t)primitive_reset);
    }
    if (reverse)
        hook_addr(reverse, (uintptr_t)primitive_reverse);
}
