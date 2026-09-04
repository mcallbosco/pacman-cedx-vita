#include "game_int_vector.h"
#include "game_patch.h"
#include <string.h>

static void (*int_vector_delete)(void *);

static void *destroy_int_vector(void *object) {
    void *begin;
    memcpy(&begin, object, sizeof(begin));
    if (begin) {
        /* Integer elements have no destruction side effects. Keep the native
         * end-pointer update before invoking its original deallocator. */
        memcpy((char *)object + 4, &begin, sizeof(begin));
        int_vector_delete(begin);
    }
    return object;
}

void game_int_vector_install_hooks(void) {
    uintptr_t base = game_patch_checked_function(
        "_ZNSt6__ndk113__vector_baseIiNS_9allocatorIiEEED2Ev", 0x4e, 0x0b7f402au);
    if (!base)
        return;
    /* The hidden vector destructor calls the annotation helper and this base.
     * Check the complete getter, annotation, trivial-element and allocator
     * helper chain before replacing either entry; other vector types stay native. */
    static const struct { int32_t offset; uint32_t size, hash; } helpers[] = {
        { -0x495e, 0x20, 0x2cff67cbu },
        { -0x72, 0x2e6, 0x4b378944u },
        { -0xf84, 0x16, 0xb0e277ecu },
        { -0x16616, 0x52, 0xb1ee61f7u },
    };
    for (size_t i = 0; i < sizeof(helpers) / sizeof(helpers[0]); ++i)
        if (!game_patch_checked_code(base + helpers[i].offset,
                "integer vector destructor helper", helpers[i].size, helpers[i].hash))
            return;
    if (!game_patch_checked_function(
            "_ZNSt6__ndk117_DeallocateCaller27__do_deallocate_handle_sizeEPvj",
            0x1c, 0xc30a5f33u))
        return;
    int_vector_delete = (void *)game_patch_checked_function(
        "_ZNSt6__ndk117_DeallocateCaller9__do_callEPv", 0x16, 0x17bcd681u);
    if (int_vector_delete) {
        hook_addr(base - 0x495e, (uintptr_t)destroy_int_vector);
        hook_addr(base, (uintptr_t)destroy_int_vector);
    }
}
