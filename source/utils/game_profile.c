#include "game_profile.h"
#include "game_patch.h"

static void **profile_instance;
static so_hook profile_hook;

static __attribute__((noinline)) void *create_profile(void) {
    return SO_CONTINUE(void *, profile_hook);
}

static void *get_profile(void) {
    void *profile = *profile_instance;
    if (profile)
        return profile;
    return create_profile();
}

static uint32_t use_batches(const uint8_t *profile) {
    return profile[0x29] & 1;
}

void game_profile_install_hooks(void) {
    uintptr_t getter = game_patch_checked_function(
        "_ZN3sys9SingletonINS_13DeviceProfileEE11GetInstanceEv", 0x48, 0x857a53beu);
    profile_instance = (void **)so_symbol(&so_mod,
        "_ZN3sys9SingletonINS_13DeviceProfileEE11s_pInstanceE");
    if (getter && profile_instance)
        profile_hook = hook_addr(getter, (uintptr_t)get_profile);

    uintptr_t batches = game_patch_checked_function(
        "_ZN3sys13DeviceProfile22UseBatchesOptimizationEv", 0x16, 0x7ecbe990u);
    if (batches)
        hook_addr(batches, (uintptr_t)use_batches);
}
