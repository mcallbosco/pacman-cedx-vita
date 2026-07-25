#ifndef PMCEDX_GAME_PATCH_H
#define PMCEDX_GAME_PATCH_H

#include <so_util/so_util.h>
#include <stdint.h>
#include "logger.h"

extern so_module so_mod;

/* FNV-1a fingerprints cover the entire original function, including its literal
 * pool. These native patches were checked against Android 1.2.0's
 * armeabi-v7a library (Build ID 282a7990ef34572c6fdcea7913840b11feaca95e).
 * A different function body is left untouched. This is a version check, not an
 * integrity/security check. */
static uintptr_t game_patch_checked_function(const char *symbol, size_t size, uint32_t hash) {
    uintptr_t addr = so_symbol(&so_mod, symbol);
    uintptr_t code = addr & ~(uintptr_t)1;
    if (!(addr & 1) || code < so_mod.text_base || size > so_mod.text_size ||
        code - so_mod.text_base > so_mod.text_size - size)
        return 0;

    const uint8_t *bytes = (const uint8_t *)code;
    uint32_t actual = 2166136261u;
    for (size_t i = 0; i < size; ++i)
        actual = (actual ^ bytes[i]) * 16777619u;
    if (actual != hash) {
        l_warn("Native patch skipped: unexpected code for %s", symbol);
        return 0;
    }
    return addr;
}

#endif
