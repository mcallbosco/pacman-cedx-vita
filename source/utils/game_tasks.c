#include "game_tasks.h"
#include "game_patch.h"
#include "game_ghost_scan.h"
#include <string.h>

static uint32_t word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static void *pointer(const void *object, size_t offset) {
    return (void *)(uintptr_t)word(object, offset);
}

static void reset_refresh(void *task) {
    uint32_t flags = word(task, 20) & ~0x80u;
    memcpy((char *)task + 20, &flags, sizeof(flags));
}

static void task_set_all_hide(void *task, uint32_t hide) {
    uint32_t flags = word(task, 20);
    flags = hide & 1 ? flags & ~4u : flags | 4u;
    memcpy((char *)task + 20, &flags, sizeof(flags));
    void *end = (char *)task + 8;
    for (void *node = pointer(task, 12); node != end; node = pointer(node, 4))
        task_set_all_hide(pointer(node, 8), hide);
}

static void install_task_visibility(void) {
    uintptr_t hide = game_patch_checked_function(
        "_ZN3sys5cTask10SetAllHideEb", 0xa0, 0xf7093952u);
    if (hide &&
        game_patch_checked_function("_ZN3sys5cTask7SetDrawEv", 0x16, 0x0933cb40u) &&
        game_patch_checked_function("_ZN3sys5cTask9ResetDrawEv", 0x16, 0xc308a260u) &&
        game_patch_checked_function("_ZN3sys5cTask17GetChildTaskBeginEv", 0x1c, 0x31ea37d6u) &&
        game_patch_checked_function("_ZN3sys5cTask15GetChildTaskEndEv", 0x1c, 0xf94903a9u))
        hook_addr(hide, (uintptr_t)task_set_all_hide);
}

static void call_task(void *task, size_t slot) {
    void (*callback)(void *) = pointer(pointer(task, 0), slot);
    game_ghost_scan_call(task, callback);
}

static void task_refresh(void *task) {
    if ((word(task, 20) & 0xc0) == 0x80)
        reset_refresh(task);
    if ((int32_t)word(task, 16) < 1)
        return;
    void *end = (char *)task + 8;
    for (void *node = pointer(task, 12); node != end; node = pointer(node, 4)) {
        call_task(pointer(node, 8), 12);
        /* A callback can change the node's value or next link. Re-read both,
         * in the same order as the native iterator code. */
        reset_refresh(pointer(node, 8));
    }
}

static void task_execute_impl(void *task, uint32_t children, uint32_t run_func,
                         uint32_t run_draw, uint32_t run_extra) {
    if (word(task, 20) & 0x40)
        return;
    if ((run_func & 1) && (word(task, 20) & 9) == 1)
        call_task(task, 8);
    if ((run_draw & 1) && (word(task, 20) & 0x42) == 2)
        call_task(task, 16);
    if ((run_extra & 1) && (word(task, 20) & 0x44) == 4)
        call_task(task, 20);
    if ((word(task, 20) & 0xc0) == 0x80)
        call_task(task, 12);
    /* The native function still traverses children if a callback marked the
     * parent dead. Only its initial dead check skips the entire execution. */
    if (!(children & 1) || (int32_t)word(task, 16) < 1)
        return;
    void *end = (char *)task + 8;
    for (void *node = pointer(task, 12); node != end; node = pointer(node, 4))
        task_execute_impl(pointer(node, 8), 1, run_func & 1, run_draw & 1, run_extra & 1);
}

static void task_execute(void *task, uint32_t children, uint32_t run_func,
                         uint32_t run_draw, uint32_t run_extra) {
    game_ghost_scan_reset();
    task_execute_impl(task, children, run_func, run_draw, run_extra);
    game_ghost_scan_reset();
}

static void (*task_node_delete)(void *);

static void cleanup_store(void *object, size_t offset, uint32_t value) {
    *(volatile uint32_t *)((char *)object + offset) = value;
}

static void *cleanup_erase(void *list, void *node) {
    void *next = pointer(node, 4);
    /* Keep unlink, count update and native deallocation in their original
     * order. The stored task pointer has a trivial destructor. */
    cleanup_store(pointer(node, 0), 4, (uintptr_t)next);
    cleanup_store(pointer(node, 4), 0, word(node, 0));
    cleanup_store(list, 8, word(list, 8) - 1);
    task_node_delete(node);
    return next;
}

static void task_cleanup(void *task) {
    if (word(task, 16)) {
        void *end = (char *)task + 8;
        void *node = pointer(task, 12);
        while (node != end) {
            void *child = pointer(node, 8);
            if (word(child, 20) & 0x40) {
                cleanup_store(child, 4, 0);
                node = cleanup_erase(end, node);
            } else {
                node = pointer(node, 4);
            }
            /* Native ExecFree caches both child and next before recursion.
             * Destructors can change sibling flags, lists or this task. */
            task_cleanup(child);
        }
    }
    if (word(task, 20) & 0x40) {
        void (*destroy)(void *) = pointer(pointer(task, 0), 4);
        destroy(task);
    }
}

static void install_task_cleanup(void) {
    uintptr_t cleanup = game_patch_checked_function(
        "_ZN3sys5cTask8ExecFreeEv", 0xd8, 0x90bfbdabu);
    if (!cleanup ||
        !game_patch_checked_function("_ZN3sys5cTask7GetDeadEv", 0x14, 0x68416a82u) ||
        !game_patch_checked_function("_ZN3sys5cTask14GetChildrenCntEv", 0x38, 0xd478c14bu) ||
        !game_patch_checked_function("_ZN3sys5cTask17GetChildTaskBeginEv", 0x1c, 0x31ea37d6u) ||
        !game_patch_checked_function("_ZN3sys5cTask15GetChildTaskEndEv", 0x1c, 0xf94903a9u) ||
        !game_patch_checked_function("_ZNSt6__ndk14listIPN3sys5cTaskENS_9allocatorIS3_EEE5eraseENS_21__list_const_iteratorIS3_PvEE", 0xa0, 0xe3f5436eu) ||
        !game_patch_checked_function("_ZNSt6__ndk116allocator_traitsINS_9allocatorINS_11__list_nodeIPN3sys5cTaskEPvEEEEE7destroyIS5_EEvRS8_PT_", 0x1e, 0xf5253164u) ||
        !game_patch_checked_function("_ZNSt6__ndk116allocator_traitsINS_9allocatorINS_11__list_nodeIPN3sys5cTaskEPvEEEEE9__destroyIS5_EEvNS_17integral_constantIbLb0EEERS8_PT_", 0x12, 0x72f798d9u) ||
        !game_patch_checked_function("_ZNSt6__ndk117_DeallocateCaller27__do_deallocate_handle_sizeEPvj", 0x1c, 0xc30a5f33u))
        return;

    /* Full iterator, count, node conversion and allocator helper bodies.
     * Validate before the later GetChildrenCnt hook replaces its entry. */
    static const struct { int32_t offset; uint32_t size, hash; } helpers[] = {
        { -0x859fa, 0x48, 0xcb83b455u },
        { -0x858ba, 0x1b0, 0x34c09cacu },
        { -0x856f4, 0x72, 0x663e74f9u },
        { 0x178, 0x1a, 0x31149973u },
        { 0x32c, 0x62, 0xfa586241u },
        { 0xa4c, 0x18, 0xc979d796u },
        { 0x1b50, 0x90, 0xd5175ebbu },
        { -0xf022a, 0xe, 0x6f470d90u },
        { -0xec136, 0xe, 0x6f470d90u },
        { -0xf281a, 0x52, 0xb1ee61f7u },
    };
    for (size_t i = 0; i < sizeof(helpers) / sizeof(helpers[0]); ++i)
        if (!game_patch_checked_code(cleanup + helpers[i].offset,
                "task cleanup helper", helpers[i].size, helpers[i].hash))
            return;

    task_node_delete = (void *)game_patch_checked_function(
        "_ZNSt6__ndk117_DeallocateCaller9__do_callEPv", 0x16, 0x17bcd681u);
    if (task_node_delete)
        hook_addr(cleanup, (uintptr_t)task_cleanup);
}

void game_tasks_install_hooks(void) {
    install_task_visibility();
    install_task_cleanup();
    uintptr_t execute = game_patch_checked_function(
        "_ZN3sys5cTask7ExecuteEbbbb", 0x198, 0x726a7f2eu);
    uintptr_t refresh = game_patch_checked_function(
        "_ZN3sys5cTask7RefreshEv", 0xb4, 0x941bc942u);
    if (!execute || !refresh ||
        !game_patch_checked_function("_ZN3sys5cTask7GetDeadEv", 0x14, 0x68416a82u) ||
        !game_patch_checked_function("_ZN3sys5cTask12ResetRefreshEv", 0x16, 0x6f2779d4u) ||
        !game_patch_checked_function("_ZN3sys5cTask14GetChildrenCntEv", 0x38, 0xd478c14bu) ||
        !game_patch_checked_function("_ZN3sys5cTask17GetChildTaskBeginEv", 0x1c, 0x31ea37d6u) ||
        !game_patch_checked_function("_ZN3sys5cTask15GetChildTaskEndEv", 0x1c, 0xf94903a9u))
        return;
    hook_addr(execute, (uintptr_t)task_execute);
    hook_addr(refresh, (uintptr_t)task_refresh);
}
