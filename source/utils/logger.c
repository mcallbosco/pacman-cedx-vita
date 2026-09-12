/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include <stdbool.h>
#include <stdatomic.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

#ifdef SOLOADER_FILE_LOGGING
#define LOG_FILE_PATH WRITABLE_PATH "debug.log"
static SceUID _log_fd = -1;
static char _log_recovery[128];
static unsigned int _log_recovery_length;
static unsigned int _log_recovery_written;

static void close_log_file(void) {
    sceIoClose(_log_fd);
    _log_fd = -1;
}

/* A suspended app can lose its open file handles. Retry only the unwritten
 * bytes once, then leave the descriptor closed for the next log entry. */
static bool write_log_bytes(const char *data, unsigned int length,
                            unsigned int *written) {
    if (_log_fd < 0) {
        _log_fd = sceIoOpen(LOG_FILE_PATH,
                            SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    }
    if (_log_fd < 0) return false;

    unsigned int remaining = length - *written;
    int ret = sceIoWrite(_log_fd, data + *written, remaining);
    if (ret == (int)remaining) return true;

    if (!_log_recovery_length) {
        _log_recovery_length = sceClibSnprintf(_log_recovery,
                sizeof(_log_recovery),
                "\n[LOGGER] write recovery result=0x%08x requested=%u\n",
                ret, remaining);
        _log_recovery_written = 0;
    }
    if (ret > 0) *written += ret;
    close_log_file();
    _log_fd = sceIoOpen(LOG_FILE_PATH,
                        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (_log_fd < 0) return false;

    remaining = length - *written;
    ret = sceIoWrite(_log_fd, data + *written, remaining);
    if (ret == (int)remaining) return true;
    if (ret > 0) *written += ret;
    close_log_file();
    return false;
}

static bool flush_log_recovery(void) {
    if (!_log_recovery_length) return true;
    if (!write_log_bytes(_log_recovery, _log_recovery_length,
                         &_log_recovery_written)) return false;
    _log_recovery_length = 0;
    _log_recovery_written = 0;
    return true;
}
#endif

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    sceClibPrintf("%s", buffer_b);

#ifdef SOLOADER_FILE_LOGGING
    /* Finish a pending recovery marker before appending the next entry. */
    if (flush_log_recovery()) {
        unsigned int written = 0;
        if (write_log_bytes(buffer_b, sceClibStrnlen(buffer_b, sizeof(buffer_b)),
                            &written)) {
            flush_log_recovery();
        }
    }
#endif

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
