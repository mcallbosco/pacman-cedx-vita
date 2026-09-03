#include "game_frame_rate.h"
#include "game_patch.h"
#include "settings.h"

#include <psp2/display.h>

static bool frame_rate_enabled, frame_rate_draw = true, frame_rate_have_tick;
static unsigned frame_rate_last_vblank;
static void **frame_rate_graphics;
static uintptr_t __attribute__((used)) frame_rate_resume;

/* Keep App::Tick's input, update and audio paths on every tick. Only its
 * BeginScene / App::Render / EndScene block runs on alternate ticks. */
static void __attribute__((used, noinline)) frame_rate_render(void *app) {
    if (!frame_rate_draw)
        return;
    typedef void (*method)(void *);
    method *vtable = *(method **)*frame_rate_graphics;
    vtable[9](*frame_rate_graphics);
    vtable = *(method **)app;
    vtable[6](app);
    vtable = *(method **)*frame_rate_graphics;
    vtable[16](*frame_rate_graphics);
}

static void __attribute__((naked)) frame_rate_bridge(void) {
    __asm__ volatile(
        "ldr r0, [sp, #20]\n"
        "push {r4, lr}\n"
        "bl frame_rate_render\n"
        "pop {r4, lr}\n"
        "ldr ip, =frame_rate_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_frame_rate_install_hooks(void) {
    if (settings_sanitize_frame_rate(setting_frameRate) != 30)
        return;
    uintptr_t tick = game_patch_checked_function(
        "_ZN3sys3App4TickEv", 0x1a0, 0x2c1807dcu);
    frame_rate_graphics = (void *)so_symbol(&so_mod, "_ZN3sys11g_pGraphicsE");
    if (!tick || !frame_rate_graphics) {
        l_warn("30 FPS unavailable for this game library; keeping 60 FPS");
        return;
    }
    frame_rate_resume = tick + 0x144;
    hook_addr(tick + 0x11e, (uintptr_t)frame_rate_bridge);
    frame_rate_enabled = true;
    l_info("30 FPS rendering enabled; gameplay updates remain at 60 Hz");
}

void game_frame_rate_begin_tick(void) {
    if (!frame_rate_enabled)
        return;
    /* A skipped draw must still consume one display tick. Swap alone cannot
     * pace these ticks. Avoid an extra wait if rendering already crossed a
     * vblank, and resume directly after stalls instead of replaying a backlog. */
    unsigned vblank = sceDisplayGetVcount();
    if (frame_rate_have_tick && vblank == frame_rate_last_vblank) {
        sceDisplayWaitVblankStart();
        vblank = sceDisplayGetVcount();
    }
    frame_rate_draw = !frame_rate_have_tick || !frame_rate_draw;
    frame_rate_have_tick = true;
    frame_rate_last_vblank = vblank;
}

bool game_frame_rate_should_render(void) {
    return frame_rate_draw;
}

int game_frame_rate_render_ticks(void) {
    return frame_rate_enabled ? 2 : 1;
}
