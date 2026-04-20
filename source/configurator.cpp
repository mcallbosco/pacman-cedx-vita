/*
 * configurator.cpp
 *
 * Standalone settings configurator launched from LiveArea.
 * Uses vita2d + PGF text for a simple controller-driven menu.
 * Reads/writes ux0:data/pacmancedx/config.txt.
 */

#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include <vita2d.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#define CONFIG_FILE "ux0:data/pacmancedx/config.txt"
#define CONFIG_DIR  "ux0:data/pacmancedx"
#define LOG_FILE    "ux0:data/pacmancedx/configurator.log"

enum {
    MSAA_OFF = 0,
    MSAA_2X  = 1,
    MSAA_4X  = 2
};

enum OptionIndex {
    OPT_MSAA = 0,
    OPT_BUILD_TYPE = 1,
    OPT_LOW_PERF = 2,
    OPT_ALL_MISSIONS = 3,
    OPT_DUMMY = 4,
    OPTION_COUNT
};

#define VISIBLE_OPTIONS 4
#define BUTTON_ROW OPTION_COUNT  /* selected_row value when focus is on buttons */

static int msaa_mode = MSAA_OFF;
static int build_type = 0;       /* 0=release, 1=debug */
static int low_performance = 0;  /* 0=off, 1=on */
static int all_missions = 1;     /* 1=unlocked, 0=normal */
static int dummy_setting = 0;    /* placeholder setting to demo scrolling */
static bool dirty = false;

static int selected_row = 0;     /* 0..OPTION_COUNT-1 = option, OPTION_COUNT = button row */
static int selected_button = 0;  /* 0 = SAVE, 1 = EXIT */
static int scroll_offset = 0;    /* first visible option index */
static vita2d_pgf *g_font = nullptr;

static void log_line(const char *fmt, ...) {
    sceIoMkdir(CONFIG_DIR, 0777);

    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    int fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0)
        return;

    sceIoWrite(fd, buffer, strlen(buffer));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

static const char *msaa_to_string(int mode) {
    switch (mode) {
        case MSAA_OFF: return "OFF";
        case MSAA_2X:  return "2X";
        case MSAA_4X:  return "4X";
        default:       return "OFF";
    }
}

static int sanitize_msaa(int mode) {
    if (mode < MSAA_OFF || mode > MSAA_4X)
        return MSAA_OFF;
    return mode;
}

static void load_settings() {
    msaa_mode = MSAA_OFF;
    build_type = 0;
    low_performance = 0;
    all_missions = 1;
    dummy_setting = 0;
    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        log_line("load_settings: %s missing, using default", CONFIG_FILE);
        return;
    }

    char key[64];
    int val;
    while (fscanf(f, "%63s %d\n", key, &val) == 2) {
        if (strcmp(key, "setting_msaaMode") == 0)
            msaa_mode = sanitize_msaa(val);
        else if (strcmp(key, "setting_buildType") == 0)
            build_type = (val == 0 || val == 1) ? val : 0;
        else if (strcmp(key, "setting_lowPerformance") == 0)
            low_performance = (val != 0) ? 1 : 0;
        else if (strcmp(key, "setting_accessAllMissions") == 0)
            all_missions = (val != 0) ? 1 : 0;
        else if (strcmp(key, "setting_dummy") == 0)
            dummy_setting = (val != 0) ? 1 : 0;
    }
    fclose(f);
    log_line("load_settings: msaa=%d build_type=%d low_perf=%d all_missions=%d dummy=%d",
             msaa_mode, build_type, low_performance, all_missions, dummy_setting);
}

static void save_settings() {
    sceIoMkdir(CONFIG_DIR, 0777);

    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) {
        log_line("save_settings: fopen failed");
        return;
    }

    fprintf(f, "setting_sampleSetting 1\n");
    fprintf(f, "setting_sampleSetting2 1\n");
    fprintf(f, "setting_msaaMode %d\n", sanitize_msaa(msaa_mode));
    fprintf(f, "setting_buildType %d\n", (build_type == 0 || build_type == 1) ? build_type : 0);
    fprintf(f, "setting_lowPerformance %d\n", low_performance ? 1 : 0);
    fprintf(f, "setting_accessAllMissions %d\n", all_missions ? 1 : 0);
    fprintf(f, "setting_dummy %d\n", dummy_setting ? 1 : 0);
    fclose(f);
    log_line("save_settings: wrote msaa=%d build_type=%d low_perf=%d all_missions=%d dummy=%d",
             msaa_mode, build_type, low_performance, all_missions, dummy_setting);
}

static bool pressed(uint32_t now, uint32_t prev, uint32_t button) {
    return (now & button) && !(prev & button);
}

static void draw_label(int x, int y, unsigned int color, float scale, const char *text) {
    if (g_font)
        vita2d_pgf_draw_text(g_font, x, y, color, scale, text);
}

static void draw_row(float x, float y, float w, const char *label, const char *value, bool selected) {
    unsigned int bg = selected ? RGBA8(242, 210, 64, 255) : RGBA8(50, 60, 76, 255);
    unsigned int fg = selected ? RGBA8(18, 18, 18, 255) : RGBA8(236, 240, 246, 255);
    unsigned int vg = selected ? RGBA8(18, 18, 18, 255) : RGBA8(216, 220, 228, 255);

    vita2d_draw_rectangle(x, y, w, 58.0f, bg);
    draw_label((int)x + 18, (int)y + 38, fg, 1.0f, label);
    if (value)
        draw_label((int)(x + w) - 174, (int)y + 38, vg, 1.0f, value);
}

static void draw_scroll_arrow_up(int cx, int cy, unsigned int color) {
    vita2d_draw_line((float)(cx - 6), (float)(cy + 4), (float)cx, (float)(cy - 4), color);
    vita2d_draw_line((float)cx, (float)(cy - 4), (float)(cx + 6), (float)(cy + 4), color);
    vita2d_draw_line((float)(cx - 6), (float)(cy + 4), (float)(cx + 6), (float)(cy + 4), color);
}

static void draw_scroll_arrow_down(int cx, int cy, unsigned int color) {
    vita2d_draw_line((float)(cx - 6), (float)(cy - 4), (float)cx, (float)(cy + 4), color);
    vita2d_draw_line((float)cx, (float)(cy + 4), (float)(cx + 6), (float)(cy - 4), color);
    vita2d_draw_line((float)(cx - 6), (float)(cy - 4), (float)(cx + 6), (float)(cy - 4), color);
}

static void render_frame() {
    vita2d_start_drawing();
    vita2d_clear_screen();

    vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(14, 18, 28, 255));
    vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 120.0f, RGBA8(20, 34, 52, 255));
    vita2d_draw_rectangle(52.0f, 98.0f, 856.0f, 2.0f, RGBA8(242, 210, 64, 255));

    draw_label(62, 52, RGBA8(244, 246, 250, 255), 1.4f, "PAC-MAN CE DX CONFIGURATION");
    draw_label(64, 146, RGBA8(188, 198, 218, 255), 1.0f, "D-Pad: navigate    Left/Right: change value");
    draw_label(64, 176, RGBA8(188, 198, 218, 255), 1.0f, "X: select/save    O: back/exit");

    struct OptionDesc {
        const char *label;
        const char *value;
    };
    OptionDesc options[OPTION_COUNT] = {
        {"MSAA ANTI-ALIASING",    msaa_to_string(msaa_mode)},
        {"BUILD TYPE",            build_type ? "DEBUG" : "RELEASE"},
        {"LOW PERFORMANCE MODE",  low_performance ? "ON" : "OFF"},
        {"UNLOCK ALL MISSIONS",   all_missions ? "ON" : "OFF"},
        {"DUMMY SETTING",         dummy_setting ? "ON" : "OFF"},
    };

    const float list_x = 96.0f;
    const float list_w = 768.0f;
    const int base_y = 200;
    const int row_height = 58;

    for (int slot = 0; slot < VISIBLE_OPTIONS; ++slot) {
        int idx = scroll_offset + slot;
        if (idx >= OPTION_COUNT)
            break;
        bool selected = (selected_row == idx);
        draw_row(list_x, (float)(base_y + slot * row_height), list_w,
                 options[idx].label, options[idx].value, selected);
    }

    unsigned int arrow_color = RGBA8(188, 198, 218, 255);
    int arrow_x = (int)(list_x + list_w + 16);
    if (scroll_offset > 0)
        draw_scroll_arrow_up(arrow_x, base_y + 14, arrow_color);
    if (scroll_offset + VISIBLE_OPTIONS < OPTION_COUNT)
        draw_scroll_arrow_down(arrow_x, base_y + VISIBLE_OPTIONS * row_height - 14, arrow_color);

    const float btn_y = 456.0f;
    const float btn_w = 376.0f;
    const float btn_h = 58.0f;
    const float left_x = 96.0f;
    const float right_x = 488.0f;

    bool save_selected = (selected_row == BUTTON_ROW && selected_button == 0);
    bool exit_selected = (selected_row == BUTTON_ROW && selected_button == 1);

    unsigned int save_bg = save_selected ? RGBA8(242, 210, 64, 255) : RGBA8(50, 60, 76, 255);
    unsigned int save_fg = save_selected ? RGBA8(18, 18, 18, 255) : RGBA8(236, 240, 246, 255);
    unsigned int exit_bg = exit_selected ? RGBA8(242, 210, 64, 255) : RGBA8(50, 60, 76, 255);
    unsigned int exit_fg = exit_selected ? RGBA8(18, 18, 18, 255) : RGBA8(236, 240, 246, 255);

    vita2d_draw_rectangle(left_x,  btn_y, btn_w, btn_h, save_bg);
    vita2d_draw_rectangle(right_x, btn_y, btn_w, btn_h, exit_bg);
    draw_label((int)left_x  + 110, (int)btn_y + 38, save_fg, 1.0f, "SAVE & EXIT");
    draw_label((int)right_x + 150, (int)btn_y + 38, exit_fg, 1.0f, "EXIT");

    draw_label(64, 536, RGBA8(194, 200, 210, 255), 1.0f,
               dirty ? "Unsaved changes. Press SAVE & EXIT to keep them."
                     : "Changes apply on next game launch.");

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void ensure_scroll_visible() {
    if (selected_row >= OPTION_COUNT)
        return;
    if (selected_row < scroll_offset)
        scroll_offset = selected_row;
    else if (selected_row >= scroll_offset + VISIBLE_OPTIONS)
        scroll_offset = selected_row - VISIBLE_OPTIONS + 1;
    if (scroll_offset < 0)
        scroll_offset = 0;
    int max_offset = OPTION_COUNT - VISIBLE_OPTIONS;
    if (max_offset < 0)
        max_offset = 0;
    if (scroll_offset > max_offset)
        scroll_offset = max_offset;
}

static void cycle_option(int idx, int direction) {
    /* direction: +1 = forward/right, -1 = backward/left. */
    switch (idx) {
        case OPT_MSAA:
            msaa_mode = (msaa_mode + (direction > 0 ? 1 : 2)) % 3;
            dirty = true;
            break;
        case OPT_BUILD_TYPE:
            build_type = build_type ? 0 : 1;
            dirty = true;
            break;
        case OPT_LOW_PERF:
            low_performance = low_performance ? 0 : 1;
            dirty = true;
            break;
        case OPT_ALL_MISSIONS:
            all_missions = all_missions ? 0 : 1;
            dirty = true;
            break;
        case OPT_DUMMY:
            dummy_setting = dummy_setting ? 0 : 1;
            dirty = true;
            break;
        default:
            break;
    }
}

int main() {
    sceIoRemove(LOG_FILE);
    log_line("main: start");

    SceAppUtilInitParam init_param;
    SceAppUtilBootParam boot_param;
    sceClibMemset(&init_param, 0, sizeof(init_param));
    sceClibMemset(&boot_param, 0, sizeof(boot_param));
    int app_ret = sceAppUtilInit(&init_param, &boot_param);
    log_line("main: sceAppUtilInit ret=0x%08X", app_ret);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    int v2d_ret = vita2d_init_advanced_with_msaa(1 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
    log_line("main: vita2d_init_advanced_with_msaa ret=%d", v2d_ret);
    g_font = vita2d_load_default_pgf();
    log_line("main: vita2d_load_default_pgf font=%p", g_font);

    load_settings();

    SceCtrlData pad;
    sceClibMemset(&pad, 0, sizeof(pad));
    uint32_t prev_buttons = 0;
    bool running = true;
    bool save_on_exit = false;

    while (running) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t buttons = pad.buttons;

        if (pressed(buttons, prev_buttons, SCE_CTRL_UP)) {
            if (selected_row == BUTTON_ROW) {
                selected_row = OPTION_COUNT - 1;
            } else if (selected_row > 0) {
                selected_row--;
            }
            ensure_scroll_visible();
        }
        if (pressed(buttons, prev_buttons, SCE_CTRL_DOWN)) {
            if (selected_row < OPTION_COUNT - 1) {
                selected_row++;
            } else if (selected_row == OPTION_COUNT - 1) {
                selected_row = BUTTON_ROW;
            }
            ensure_scroll_visible();
        }

        if (pressed(buttons, prev_buttons, SCE_CTRL_LEFT)) {
            if (selected_row == BUTTON_ROW)
                selected_button = 0;
            else
                cycle_option(selected_row, -1);
        }
        if (pressed(buttons, prev_buttons, SCE_CTRL_RIGHT)) {
            if (selected_row == BUTTON_ROW)
                selected_button = 1;
            else
                cycle_option(selected_row, +1);
        }

        if (pressed(buttons, prev_buttons, SCE_CTRL_CROSS)) {
            if (selected_row == BUTTON_ROW) {
                save_on_exit = (selected_button == 0);
                running = false;
            } else {
                cycle_option(selected_row, +1);
            }
        }

        if (pressed(buttons, prev_buttons, SCE_CTRL_CIRCLE)) {
            save_on_exit = false;
            running = false;
        }

        render_frame();
        prev_buttons = buttons;
    }

    if (save_on_exit)
        save_settings();

    vita2d_wait_rendering_done();
    if (g_font)
        vita2d_free_pgf(g_font);
    vita2d_fini();

    log_line("main: exit save_on_exit=%d dirty=%d", save_on_exit ? 1 : 0, dirty ? 1 : 0);
    sceKernelExitProcess(0);
    return 0;
}
