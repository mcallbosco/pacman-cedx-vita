/*
 * main.c
 *
 * ARMv7 Shared Libraries loader. PAC-MAN Championship Edition DX
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2021-2023 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/settings.h"
#include "utils/logger.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"
#include "reimpl/egl.h"

#include <vitasdk.h>
#include <kubridge.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024; /* 4MB stack for shader compilation (SceShaccCg is stack-heavy) */

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 24 * 1024 * 1024;
#endif

so_module so_mod;
so_module fmod_mod;

/*
 * PAC-MAN CE DX initialization sequence (from smali analysis):
 *
 * 1. Activity.onCreate:
 *    - initFileSystem(obbPath, filesDir)
 *    - nativeSetBuildType(0)
 *    - nativeSetIsTelevisionDevice(true)
 *    - nativeSetHasImmersion(false)
 *    - FMOD.init(context)
 *    - nativeOnCreate()
 *
 * 2. Renderer.onSurfaceCreated:
 *    - init(width, height)
 *    - nativeSetDPI(dpi)
 *    - nativeSetButtonsLayoutConfig(config)
 *    - SurfaceCreated()
 *
 * 3. Renderer.onDrawFrame:
 *    - step()  (called every frame)
 *
 * 4. Input via:
 *    - DealInputDate(buttonMask, leverMask) for controller
 *    - touchEvent(action, x, y) for touch
 *    - inputMfiEvent(button, pressed) for MFi controller
 *
 * All JNI functions are static - no JNI_OnLoad needed.
 */

/* Function pointers from libPacmanCE.so (docomotab variant) */
static void (*initFileSystem)(void *env, void *obj, void *obbPath, void *filesDir);
static void (*init)(void *env, void *obj, int width, int height);
static void (*step_func)(void *env, void *obj);
static void (*SurfaceCreated)(void *env, void *obj);
static void (*Release)(void *env, void *obj);
static void (*pauseEvent)(void *env, void *obj);
static void (*resumeEvent)(void *env, void *obj);
static void (*touchEvent)(void *env, void *obj, int action, int x, int y);
static void (*multiTouchEvent)(void *env, void *obj, int action, int x1, int y1, int x2, int y2, int count);
static void (*DealInputDate)(void *env, void *obj, int buttonMask, int leverMask);
static void (*inputMfiEvent)(void *env, void *obj, int button, int pressed);
static void (*nativeOnCreate)(void *env, void *obj);
static void (*nativeOnDestroy)(void *env, void *obj);
static void (*nativeOnPause)(void *env, void *obj);
static void (*nativeOnResume)(void *env, void *obj);
static void (*nativeSetBuildType)(void *env, void *obj, int type);
static void (*nativeSetDPI)(void *env, void *obj, int dpi);
static void (*nativeSetButtonsLayoutConfig)(void *env, void *obj, int config);
static void (*nativeSetIsTelevisionDevice)(void *env, void *obj, int isTV);
static void (*nativeSetHasImmersion)(void *env, void *obj, int hasImmersion);
static void (*nativeControllerStatusChanged)(void *env, void *obj, int connected);
static void (*nativeBackButton)(void *env, void *obj);
static void (*nativeRefreshContext)(void *env, void *obj);
static int  (*GetMenuState)(void *env, void *obj);
static int  (*GetMenuMode)(void *env, void *obj);
static int  (*IsGamePause)(void *env, void *obj);
static int  (*ExitGame)(void *env, void *obj);

/*
 * PAC-MAN CE DX controller button bitmask (from PacmanCEActivity.smali)
 * These are the IKEY_PAD_* constants used in DealInputDate():
 */
#define IKEY_PAD_BUTTON  0x1
#define IKEY_PAD_LEVER   0x2
#define IKEY_PAD_RIGHT   0x4
#define IKEY_PAD_LEFT    0x8
#define IKEY_PAD_DOWN    0x10
#define IKEY_PAD_UP      0x20
#define IKEY_PAD_Z       0x40
#define IKEY_PAD_Y       0x80
#define IKEY_PAD_X       0x100
#define IKEY_PAD_R3      0x200
#define IKEY_PAD_SELECT  0x80000
#define IKEY_PAD_L3      0x400
#define IKEY_PAD_R2      0x800
#define IKEY_PAD_L2      0x1000
#define IKEY_PAD_R1      0x2000
#define IKEY_PAD_L1      0x4000
#define IKEY_PAD_D       0x8000
#define IKEY_PAD_C       0x10000
#define IKEY_PAD_A       0x40000
#define IKEY_PAD_B       0x20000
#define IKEY_PAD_START   0x100000
#define IKEY_PAD_BACK    0x200000

/* Analog stick dead zone for 4-way digital conversion */
#define ANALOG_DEADZONE 50

/*
 * MFi button IDs used by inputMfiEvent (decoded from inputMfiEventProcess switch table).
 * The game uses custom IDs starting at 0x40d, NOT standard Android keycodes.
 */
#define MFI_BUTTON_A     1037  /* 0x40d — confirm / action */
#define MFI_DPAD_LEFT    1061  /* 0x425 */
#define MFI_DPAD_UP      1062  /* 0x426 */
#define MFI_DPAD_RIGHT   1063  /* 0x427 */
#define MFI_DPAD_DOWN    1064  /* 0x428 */
#define MFI_MENU         1060  /* 0x424 — sets a flag (back/menu) */
#define MFI_L2           1070  /* 0x42e */

static void resolve_game_symbols(void) {
    /* Use the docomotab variant - this is the Google Play version */
    #define SYM(name) \
        so_symbol(&so_mod, "Java_com_namcobandaigames_pacmancedx_docomotab_PacmanCEJniLib_" #name)

    initFileSystem        = (void *)SYM(initFileSystem);
    init                  = (void *)SYM(init);
    step_func             = (void *)SYM(step);
    SurfaceCreated        = (void *)SYM(SurfaceCreated);
    Release               = (void *)SYM(Release);
    pauseEvent            = (void *)SYM(pauseEvent);
    resumeEvent           = (void *)SYM(resumeEvent);
    touchEvent            = (void *)SYM(touchEvent);
    multiTouchEvent       = (void *)SYM(multiTouchEvent);
    DealInputDate         = (void *)SYM(DealInputDate);
    inputMfiEvent         = (void *)SYM(inputMfiEvent);
    nativeOnCreate        = (void *)SYM(nativeOnCreate);
    nativeOnDestroy       = (void *)SYM(nativeOnDestroy);
    nativeOnPause         = (void *)SYM(nativeOnPause);
    nativeOnResume        = (void *)SYM(nativeOnResume);
    nativeSetBuildType    = (void *)SYM(nativeSetBuildType);
    nativeSetDPI          = (void *)SYM(nativeSetDPI);
    nativeSetButtonsLayoutConfig = (void *)SYM(nativeSetButtonsLayoutConfig);
    nativeSetIsTelevisionDevice  = (void *)SYM(nativeSetIsTelevisionDevice);
    nativeSetHasImmersion = (void *)SYM(nativeSetHasImmersion);
    nativeControllerStatusChanged = (void *)SYM(nativeControllerStatusChanged);
    nativeBackButton      = (void *)SYM(nativeBackButton);
    nativeRefreshContext  = (void *)SYM(nativeRefreshContext);
    GetMenuState          = (void *)SYM(GetMenuState);
    GetMenuMode           = (void *)SYM(GetMenuMode);
    IsGamePause           = (void *)SYM(IsGamePause);
    ExitGame              = (void *)SYM(ExitGame);

    #undef SYM

    l_info("Symbols resolved: initFileSystem=%p init=%p step=%p SurfaceCreated=%p",
           initFileSystem, init, step_func, SurfaceCreated);
    l_info("  DealInputDate=%p touchEvent=%p inputMfiEvent=%p",
           DealInputDate, touchEvent, inputMfiEvent);
}

/*
 * Input: Build controller button/lever bitmask from Vita controls
 *
 * DealInputDate(buttonMask, leverMask) is called each frame.
 * buttonMask uses IKEY_PAD_* constants above.
 * leverMask appears to encode analog stick position.
 */
/*
 * MFi input: send press/release events for button state changes.
 * inputMfiEvent(env, cls, button_id, pressed) fires per-button.
 */
static uint32_t prev_buttons = 0;

static void mfi_send_if_changed(uint32_t vita_btn, int mfi_id,
                                uint32_t cur, uint32_t prev) {
    int now  = (cur  & vita_btn) ? 1 : 0;
    int was  = (prev & vita_btn) ? 1 : 0;
    if (now == was || !inputMfiEvent) return;
    /* MFI_MENU's handler (sys::InputPad::inputMfiEventProcess @ libPacmanCE.so
     * +0x212b78) unconditionally sets the global "back key" flag to 1 on both
     * press AND release. Forwarding the release event therefore re-fires the
     * back action a second time per tap, which surfaces as the pause menu
     * reappearing after the user dismisses it. Only forward the press edge. */
    if (mfi_id == MFI_MENU && now == 0) return;
    inputMfiEvent(&jni, NULL, mfi_id, now);
}

static void process_input(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    /* Build a unified button state (hw buttons + analog stick as dpad) */
    uint32_t cur = pad.buttons;

    int lx = (int)pad.lx - 128;
    int ly = (int)pad.ly - 128;
    if (abs(lx) > ANALOG_DEADZONE || abs(ly) > ANALOG_DEADZONE) {
        if (abs(lx) > abs(ly)) {
            if (lx < -ANALOG_DEADZONE) cur |= SCE_CTRL_LEFT;
            if (lx > ANALOG_DEADZONE)  cur |= SCE_CTRL_RIGHT;
        } else {
            if (ly < -ANALOG_DEADZONE) cur |= SCE_CTRL_UP;
            if (ly > ANALOG_DEADZONE)  cur |= SCE_CTRL_DOWN;
        }
    }

    /* Send MFi press/release events for changed buttons */
    mfi_send_if_changed(SCE_CTRL_CROSS,    MFI_BUTTON_A,   cur, prev_buttons);
    mfi_send_if_changed(SCE_CTRL_UP,       MFI_DPAD_UP,    cur, prev_buttons);
    mfi_send_if_changed(SCE_CTRL_DOWN,     MFI_DPAD_DOWN,  cur, prev_buttons);
    mfi_send_if_changed(SCE_CTRL_LEFT,     MFI_DPAD_LEFT,  cur, prev_buttons);
    mfi_send_if_changed(SCE_CTRL_RIGHT,    MFI_DPAD_RIGHT, cur, prev_buttons);
    mfi_send_if_changed(SCE_CTRL_START,    MFI_MENU,       cur, prev_buttons);
    mfi_send_if_changed(SCE_CTRL_LTRIGGER, MFI_L2,         cur, prev_buttons);

    prev_buttons = cur;

    /* Also send DealInputDate bitmask for any code paths that use it */
    int buttonMask = 0;
    if (cur & SCE_CTRL_UP)       buttonMask |= IKEY_PAD_UP;
    if (cur & SCE_CTRL_DOWN)     buttonMask |= IKEY_PAD_DOWN;
    if (cur & SCE_CTRL_LEFT)     buttonMask |= IKEY_PAD_LEFT;
    if (cur & SCE_CTRL_RIGHT)    buttonMask |= IKEY_PAD_RIGHT;
    if (cur & SCE_CTRL_CROSS)    buttonMask |= IKEY_PAD_A;
    if (cur & SCE_CTRL_SQUARE)   buttonMask |= IKEY_PAD_C;
    if (cur & SCE_CTRL_TRIANGLE) buttonMask |= IKEY_PAD_D;
    if (cur & SCE_CTRL_LTRIGGER) buttonMask |= IKEY_PAD_L1;
    if (cur & SCE_CTRL_RTRIGGER) buttonMask |= IKEY_PAD_R1;
    if (cur & SCE_CTRL_START)    buttonMask |= IKEY_PAD_START;
    if (cur & SCE_CTRL_SELECT)   buttonMask |= IKEY_PAD_SELECT;

    if (DealInputDate)
        DealInputDate(&jni, NULL, buttonMask, 0);
}

/* Controls callbacks (required by boilerplate) */
void controls_handler_key(int32_t keycode, ControlsAction action) {
    (void)keycode;
    (void)action;
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    if (touchEvent) {
        int android_action = (action == CONTROLS_ACTION_DOWN) ? 0 :
                             (action == CONTROLS_ACTION_UP) ? 1 : 2;
        touchEvent(&jni, NULL, android_action, (int)x, (int)y);
    }
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    (void)which; (void)x; (void)y; (void)action;
}

/*
 * Abort handler: captures crash register state to debug.log before dying.
 * Registered via kubridge on real hardware — gives us PC, LR, FAR for SIGSEGV.
 */
static void crash_abort_handler(KuKernelAbortContext *ctx) {
    SceUID fd = sceIoOpen(WRITABLE_PATH "debug.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        char buf[512];
        int len = sceClibSnprintf(buf, sizeof(buf),
            "\n=== ABORT HANDLER ===\n"
            "Type: %s\n"
            "PC:  0x%08X\n"
            "LR:  0x%08X\n"
            "SP:  0x%08X\n"
            "FAR: 0x%08X (faulting address)\n"
            "FSR: 0x%08X\n"
            "SPSR: 0x%08X\n"
            "R0=0x%08X R1=0x%08X R2=0x%08X R3=0x%08X\n"
            "R4=0x%08X R5=0x%08X R6=0x%08X R7=0x%08X\n"
            "R8=0x%08X R9=0x%08X R10=0x%08X R11=0x%08X R12=0x%08X\n",
            ctx->abortType == KU_KERNEL_ABORT_TYPE_DATA_ABORT ? "DATA ABORT" : "PREFETCH ABORT",
            ctx->pc, ctx->lr, ctx->sp, ctx->FAR, ctx->FSR, ctx->SPSR,
            ctx->r0, ctx->r1, ctx->r2, ctx->r3,
            ctx->r4, ctx->r5, ctx->r6, ctx->r7,
            ctx->r8, ctx->r9, ctx->r10, ctx->r11, ctx->r12);
        sceIoWrite(fd, buf, len);

        /* Print .so relative addresses for easy lookup */
        extern so_module so_mod;
        extern so_module fmod_mod;
        uint32_t pc = ctx->pc;
        uint32_t lr = ctx->lr;
        uint32_t far_addr = ctx->FAR;
        len = sceClibSnprintf(buf, sizeof(buf),
            "so_mod.text_base=0x%08X size=0x%X\n"
            "fmod_mod.text_base=0x%08X size=0x%X\n"
            "PC-so_mod=0x%08X  LR-so_mod=0x%08X  FAR-so_mod=0x%08X\n"
            "PC-fmod=0x%08X  LR-fmod=0x%08X\n"
            "=== END ABORT ===\n",
            so_mod.text_base, so_mod.text_size,
            fmod_mod.text_base, fmod_mod.text_size,
            pc - so_mod.text_base, lr - so_mod.text_base, far_addr - so_mod.text_base,
            pc - fmod_mod.text_base, lr - fmod_mod.text_base);
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
    /* Also print to console */
    sceClibPrintf("ABORT: type=%d PC=0x%08X LR=0x%08X FAR=0x%08X\n",
                  ctx->abortType, ctx->pc, ctx->lr, ctx->FAR);

    /* Exit cleanly instead of returning (which re-executes the faulting instruction in a loop) */
    sceKernelExitProcess(0);
}

int main() {
    SceAppUtilInitParam appUtilParam;
    SceAppUtilBootParam appUtilBootParam;
    memset(&appUtilParam, 0, sizeof(SceAppUtilInitParam));
    memset(&appUtilBootParam, 0, sizeof(SceAppUtilBootParam));
    sceAppUtilInit(&appUtilParam, &appUtilBootParam);

    /* Truncate debug log for this run */
    {
        SceUID fd = sceIoOpen(WRITABLE_PATH "debug.log",
                              SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) sceIoClose(fd);
    }

    soloader_init_all();

    /* Register abort handler — captures crash PC/LR/FAR to debug.log.
     * Requires kubridge v0.3+ (ur0:/tai/kubridge.skprx). */
    {
        int ret = kuKernelRegisterAbortHandler(crash_abort_handler, NULL, NULL);
        l_info("kuKernelRegisterAbortHandler: %d", ret);
    }

    resolve_game_symbols();

    /* Initialize controller */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    /*
     * Step 1: Initialize file system
     * initFileSystem(obbPath, filesDir)
     * obbPath = path to OBB/data, filesDir = writable save directory
     *
     * IMPORTANT: JNI String parameters must be proper JavaString* objects,
     * not raw C strings! The native code calls GetStringUTFChars() on them.
     */
    /* ===== Load-time profiling =====
     * All per-call counters and phase timing are compiled out unless
     * ENABLE_IO_PROFILING is defined — saves ~150-200 ms cold-start on
     * release builds. Toggle via -DENABLE_IO_PROFILING=ON to re-enable
     * measurement after a change. */
#ifdef ENABLE_IO_PROFILING
    extern uint64_t g_prof_open_count, g_prof_open_us;
    extern uint64_t g_prof_read_count, g_prof_read_bytes, g_prof_read_us;
    extern uint64_t g_prof_png_bytes, g_prof_png_us;
    extern uint64_t g_prof_ogg_bytes, g_prof_ogg_us;
    extern uint64_t g_prof_other_bytes, g_prof_other_us;
    uint64_t _prof_phase_t0;
    #define PROF_PHASE_START() (_prof_phase_t0 = sceKernelGetProcessTimeWide())
    #define PROF_PHASE_END(name) l_info("[PROF] %-24s %6llu ms", name, \
        (unsigned long long)((sceKernelGetProcessTimeWide() - _prof_phase_t0) / 1000))
    uint64_t _prof_total_t0 = sceKernelGetProcessTimeWide();
#else
    #define PROF_PHASE_START() ((void)0)
    #define PROF_PHASE_END(name) ((void)0)
#endif

    if (initFileSystem) {
        l_info("Calling initFileSystem...");
        /* Both obbPath and filesDir use DATA_PATH_NOSLASH because the native
         * code constructs asset paths as "%s/assets/%s" from filesDir.
         * e.g. filesDir="ux0:data/pacmancedx" -> "ux0:data/pacmancedx/assets/data/shader/..." */
        jstring obbPath = (*(&jni))->NewStringUTF(&jni, DATA_PATH_NOSLASH);
        jstring filesDir = (*(&jni))->NewStringUTF(&jni, DATA_PATH_NOSLASH);
        PROF_PHASE_START();
        initFileSystem(&jni, NULL, obbPath, filesDir);
        PROF_PHASE_END("initFileSystem");
        l_info("initFileSystem done");
    }

    /*
     * Step 2: Configure device settings
     */
    if (nativeSetBuildType) {
        nativeSetBuildType(&jni, NULL, setting_buildType);
        l_info("nativeSetBuildType(%d)", setting_buildType);
    }
    if (nativeSetIsTelevisionDevice) {
        nativeSetIsTelevisionDevice(&jni, NULL, 1); /* Force TV profile */
        l_info("nativeSetIsTelevisionDevice(true)");
    }
    if (nativeSetHasImmersion) {
        nativeSetHasImmersion(&jni, NULL, 0); /* No haptics */
        l_info("nativeSetHasImmersion(false)");
    }

    /*
     * Step 3: nativeOnCreate
     */
    if (nativeOnCreate) {
#ifdef ENABLE_IO_PROFILING
        uint64_t io_opens0 = g_prof_open_count;
        uint64_t io_bytes0 = g_prof_read_bytes;
        uint64_t io_read_us0 = g_prof_read_us;
#endif
        PROF_PHASE_START();
        nativeOnCreate(&jni, NULL);
        PROF_PHASE_END("nativeOnCreate");
#ifdef ENABLE_IO_PROFILING
        l_info("[PROF]   during nativeOnCreate: opens=%llu bytes=%llu KB fread=%llu ms",
               (unsigned long long)(g_prof_open_count - io_opens0),
               (unsigned long long)((g_prof_read_bytes - io_bytes0) / 1024),
               (unsigned long long)((g_prof_read_us - io_read_us0) / 1000));
#endif
        l_info("nativeOnCreate done");
    }

    /*
     * Step 4: Initialize GL and call init(width, height)
     */
    PROF_PHASE_START();
    gl_init();
    egl_mark_gl_initialized(); /* Prevent double-init if native code calls eglInitialize */
    PROF_PHASE_END("gl_init");
    l_info("GL initialized (960x544)");

    if (init) {
#ifdef ENABLE_IO_PROFILING
        extern void prof_timeline_enable(void);
        prof_timeline_enable();
        uint64_t io_opens0 = g_prof_open_count;
        uint64_t io_bytes0 = g_prof_read_bytes;
#endif
        PROF_PHASE_START();
        init(&jni, NULL, 960, 544);
        PROF_PHASE_END("init(960,544)");
#ifdef ENABLE_IO_PROFILING
        l_info("[PROF]   during init: opens=%llu bytes=%llu KB",
               (unsigned long long)(g_prof_open_count - io_opens0),
               (unsigned long long)((g_prof_read_bytes - io_bytes0) / 1024));
#endif
        l_info("init(960, 544) done");
    }

    if (nativeSetDPI) {
        nativeSetDPI(&jni, NULL, 220); /* Vita ~220 DPI */
        l_info("nativeSetDPI(220)");
    }

    if (nativeSetButtonsLayoutConfig) {
        nativeSetButtonsLayoutConfig(&jni, NULL, 0);
        l_info("nativeSetButtonsLayoutConfig(0)");
    }

    /*
     * Step 5: SurfaceCreated
     */
    if (SurfaceCreated) {
#ifdef ENABLE_IO_PROFILING
        uint64_t io_opens0 = g_prof_open_count;
        uint64_t io_bytes0 = g_prof_read_bytes;
#endif
        PROF_PHASE_START();
        SurfaceCreated(&jni, NULL);
        PROF_PHASE_END("SurfaceCreated");
#ifdef ENABLE_IO_PROFILING
        l_info("[PROF]   during SurfaceCreated: opens=%llu bytes=%llu KB",
               (unsigned long long)(g_prof_open_count - io_opens0),
               (unsigned long long)((g_prof_read_bytes - io_bytes0) / 1024));
#endif
        l_info("SurfaceCreated done");
    }

    /*
     * Step 6: Notify controller connected
     */
    if (nativeControllerStatusChanged) {
        nativeControllerStatusChanged(&jni, NULL, 1); /* Controller connected */
        l_info("nativeControllerStatusChanged(true)");
    }

    if (resumeEvent) {
        resumeEvent(&jni, NULL);
        l_info("resumeEvent done");
    }

    if (nativeOnResume) {
        nativeOnResume(&jni, NULL);
        l_info("nativeOnResume done");
    }

#ifdef ENABLE_IO_PROFILING
    /* ===== Startup profiling summary =====
     * Tells us where the 40s is actually going. */
    uint64_t total_ms = (sceKernelGetProcessTimeWide() - _prof_total_t0) / 1000;
    l_info("[PROF] ============ STARTUP SUMMARY ============");
    l_info("[PROF] total startup:            %6llu ms", (unsigned long long)total_ms);
    l_info("[PROF] file opens:               %6llu (fopen %llu ms)",
           (unsigned long long)g_prof_open_count,
           (unsigned long long)(g_prof_open_us / 1000));
    l_info("[PROF] file reads:               %6llu (%llu KB, fread %llu ms)",
           (unsigned long long)g_prof_read_count,
           (unsigned long long)(g_prof_read_bytes / 1024),
           (unsigned long long)(g_prof_read_us / 1000));
    l_info("[PROF]   png:                    %6llu KB (%llu ms)",
           (unsigned long long)(g_prof_png_bytes / 1024),
           (unsigned long long)(g_prof_png_us / 1000));
    l_info("[PROF]   ogg:                    %6llu KB (%llu ms)",
           (unsigned long long)(g_prof_ogg_bytes / 1024),
           (unsigned long long)(g_prof_ogg_us / 1000));
    l_info("[PROF]   other:                  %6llu KB (%llu ms)",
           (unsigned long long)(g_prof_other_bytes / 1024),
           (unsigned long long)(g_prof_other_us / 1000));
    l_info("[PROF] =========================================");
#endif

    l_info("Entering main render loop");

    /*
     * Step 7: Main loop - call step() each frame
     */
#ifdef DEBUG_OPENGL
    extern int gl_dbg_frame;
#endif
    int frame_count = 0;
#ifdef ENABLE_IO_PROFILING
    /* Track cumulative I/O and step time across all frames so we can see
     * the full loading sequence (at 60fps, 25s of load = 1500 frames). */
    uint64_t _prof_frames_t0 = sceKernelGetProcessTimeWide();
    /* Settle detection: when we see N consecutive fast (<=20ms) frames
     * with no file opens, print a "load-complete" summary exactly once. */
    int      _prof_fast_streak = 0;
    int      _prof_load_complete_printed = 0;
    int      _prof_last_summary_frame = 0;
#endif
    while (1) {
        process_input();

        /* Auto-skip intro: on Vita, the splash.runnybin animation plays for
         * ~15 seconds before the main menu appears — pure waiting time. We
         * synthesize a CROSS (Android BUTTON_A, keycode 96) press during
         * the first 120 frames (2s) of the render loop, which the splash
         * state accepts as "skip intro". Inject a down/up pair every 10
         * frames so the game sees a fresh edge as soon as it's ready. */
        if (frame_count < 120 && (frame_count % 10) == 0) {
            controls_handler_key(96 /* AKEYCODE_BUTTON_A */, CONTROLS_ACTION_DOWN);
            controls_handler_key(96 /* AKEYCODE_BUTTON_A */, CONTROLS_ACTION_UP);
        }

#ifdef DEBUG_OPENGL
        gl_dbg_frame = frame_count;
#endif
#ifdef ENABLE_IO_PROFILING
        /* Per-frame timing for every frame until loading settles. */
        uint64_t _frame_t0 = sceKernelGetProcessTimeWide();
        uint64_t _frame_open0 = g_prof_open_count;
        uint64_t _frame_read0 = g_prof_read_us;
        uint64_t _frame_bytes0 = g_prof_read_bytes;
#endif

        if (step_func)
            step_func(&jni, NULL);

#ifdef ENABLE_IO_PROFILING
        uint64_t dt_us = sceKernelGetProcessTimeWide() - _frame_t0;
        uint64_t frame_opens = g_prof_open_count - _frame_open0;
        /* Log any frame that is slow OR opened files. */
        if (dt_us > 20000 || frame_opens > 0) {
            l_info("[PROF] Frame %d: step %llu ms | +opens=%llu +read=%llu ms +bytes=%llu KB",
                   frame_count + 1,
                   (unsigned long long)(dt_us / 1000),
                   (unsigned long long)frame_opens,
                   (unsigned long long)((g_prof_read_us - _frame_read0) / 1000),
                   (unsigned long long)((g_prof_read_bytes - _frame_bytes0) / 1024));
        }

        /* Periodic cumulative summary every 300 frames while loading. */
        if (!_prof_load_complete_printed &&
            frame_count - _prof_last_summary_frame >= 300) {
            _prof_last_summary_frame = frame_count;
            uint64_t total_ms = (sceKernelGetProcessTimeWide() - _prof_frames_t0) / 1000;
            l_info("[PROF] --- interim @ frame %d: %llu ms elapsed, "
                   "opens=%llu, read=%llu KB (%llu ms), png=%llu KB, other=%llu KB ---",
                   frame_count + 1,
                   (unsigned long long)total_ms,
                   (unsigned long long)g_prof_open_count,
                   (unsigned long long)(g_prof_read_bytes / 1024),
                   (unsigned long long)(g_prof_read_us / 1000),
                   (unsigned long long)(g_prof_png_bytes / 1024),
                   (unsigned long long)(g_prof_other_bytes / 1024));
        }

        /* Detect "loading done": 900 consecutive frames (15s @ 60fps) with
         * no file opens and each <=20ms. The long streak avoids tripping
         * during the splash/intro animation, which runs at 60fps with no
         * I/O for ~10 seconds before the real menu load at frame ~1069. */
        if (dt_us <= 20000 && frame_opens == 0) {
            _prof_fast_streak++;
        } else {
            _prof_fast_streak = 0;
        }
        if (!_prof_load_complete_printed && _prof_fast_streak >= 900) {
            _prof_load_complete_printed = 1;
            uint64_t total_ms = (sceKernelGetProcessTimeWide() - _prof_frames_t0) / 1000;
            l_info("[PROF] ===== LOAD COMPLETE (settled at frame %d) =====",
                   frame_count + 1);
            l_info("[PROF] loading frames took:     %6llu ms",
                   (unsigned long long)total_ms);
            l_info("[PROF] cumulative opens:        %6llu",
                   (unsigned long long)g_prof_open_count);
            l_info("[PROF] cumulative read bytes:   %6llu KB",
                   (unsigned long long)(g_prof_read_bytes / 1024));
            l_info("[PROF] cumulative fread time:   %6llu ms",
                   (unsigned long long)(g_prof_read_us / 1000));
            l_info("[PROF]   png:  %6llu KB (%llu ms)",
                   (unsigned long long)(g_prof_png_bytes / 1024),
                   (unsigned long long)(g_prof_png_us / 1000));
            l_info("[PROF]   ogg:  %6llu KB (%llu ms)",
                   (unsigned long long)(g_prof_ogg_bytes / 1024),
                   (unsigned long long)(g_prof_ogg_us / 1000));
            l_info("[PROF]   other:%6llu KB (%llu ms)",
                   (unsigned long long)(g_prof_other_bytes / 1024),
                   (unsigned long long)(g_prof_other_us / 1000));
            l_info("[PROF] ============================================");
            extern void prof_timeline_disable(void);
            prof_timeline_disable();
        }
#endif /* ENABLE_IO_PROFILING */

#ifdef DEBUG_OPENGL
        {
            extern int frame_seq_target, frame_seq_logged;
            if (gl_dbg_frame == frame_seq_target && !frame_seq_logged) {
                l_info("[seq] f%d === SWAP ===", gl_dbg_frame);
                frame_seq_logged = 1;
            }
        }
#endif
        gl_swap();

        frame_count++;
        {
            GLenum err = glGetError();
            if (frame_count <= 120 || frame_count % 300 == 0 || err != GL_NO_ERROR)
                l_info("Frame %d, GL error: 0x%x", frame_count, err);
        }
    }

    sceKernelExitDeleteThread(0);
}
