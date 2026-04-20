/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <reimpl/controls.h>

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>
#include <psp2/io/stat.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Base address for the Android .so files
#define FMOD_LOAD_ADDRESS 0x98000000
#define LOAD_ADDRESS      0x99000000

extern so_module so_mod;
extern so_module fmod_mod;

typedef int (*jni_onload_fn)(JavaVM *vm, void *reserved);

static void call_jni_onload_if_present(so_module *mod, const char *module_name) {
    uintptr_t sym = so_symbol(mod, "JNI_OnLoad");
    if (!sym)
        return;

    int ver = ((jni_onload_fn)sym)(&jvm, NULL);
    l_audio("[AUDIO][JNI] %s JNI_OnLoad returned 0x%08X", module_name, (unsigned int)ver);
}

void soloader_init_all() {
    // Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Overclock CPU/GPU for best performance
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    /* Initialize FalsoJNI before calling any JNI_OnLoad handlers. */
    jni_init();
    l_success("FalsoJNI initialized.");

    // Create writable directories for save data and shader cache.
    // DATA_PATH is read-only (app0:), so the shader cache lives on ux0:.
    sceIoMkdir("ux0:data/pacmancedx", 0777);
    sceIoMkdir(SAVE_PATH, 0777);
    sceIoMkdir(SHADER_CACHE_PATH_NOSLASH, 0777);

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (!file_exists(FMOD_SO_PATH)) {
        fatal_error("FMOD library not found. Please make sure that you have "
                    "%s file exactly at that path.", FMOD_SO_PATH);
    }

    /*
     * Load FMOD first since libPacmanCE.so depends on it.
     * The so_util loader resolves symbols from previously loaded modules.
     */
    l_info("Loading FMOD...");
    if (so_file_load(&fmod_mod, FMOD_SO_PATH, FMOD_LOAD_ADDRESS) < 0) {
        fatal_error("Error: could not load %s.", FMOD_SO_PATH);
    }
    so_relocate(&fmod_mod);
    l_success("FMOD relocated.");
    resolve_imports(&fmod_mod);
    l_success("FMOD imports resolved.");
    so_flush_caches(&fmod_mod);
    so_initialize(&fmod_mod);
    call_jni_onload_if_present(&fmod_mod, "libfmod.so");
    l_success("FMOD loaded and initialized.");

    l_info("Loading libPacmanCE.so...");
    if (so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS) < 0) {
        fatal_error("Error: could not load %s.", SO_PATH);
    }

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    so_patch();
    l_success("SO patched.");

    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&so_mod);
    call_jni_onload_if_present(&so_mod, "libPacmanCE.so");
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    controls_init();
    l_success("Controls initialized.");
}
