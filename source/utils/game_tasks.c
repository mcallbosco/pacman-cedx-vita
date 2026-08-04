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

void game_tasks_install_hooks(void) {
    install_task_visibility();
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
