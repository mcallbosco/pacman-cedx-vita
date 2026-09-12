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
#include "utils/settings.h"

#define CONFIG_DIR  "ux0:data/pacmancedx"
#ifdef DEBUG_SOLOADER
#define LOG_FILE    "ux0:data/pacmancedx/configurator.log"
#endif

enum OptionIndex {
    OPT_GAMEPLAY_SPEED = 0,
    OPT_PC_RULES,
    OPT_LANGUAGE,
    OPT_MSAA,
    OPT_BUILD_TYPE,
    OPT_OPEN_PRESETS,
    OPT_MOTION_BLUR_ENABLED,
    OPT_MOTION_BLUR,
    OPT_GHOST_AFTERIMAGES,
    OPT_GHOST_TRAILS,
    OPT_GHOST_CHAIN_TRAIL,
    OPT_GHOST_EYE_TRAILS,
    OPT_MAZE_WOBBLE,
    OPT_GHOST_EAT_OUTLINE,
    OPT_PACMAN_LIGHT,
    OPT_POWER_PALETTE,
    OPT_POWER_FLASH,
    OPT_POWER_PULSE,
    OPT_MAZE_GLOW,
    OPT_IMPACT_RIPPLES,
    OPT_DANGER_ZOOM,
    OPT_ALL_CONTENT,
    OPT_GHOST_EAT_PARTICLES,
    OPT_FRAME_RATE,
    OPT_RESOLUTION,
    OPT_NATIVE_UI,
    OPT_OPEN_GAMEPLAY,
    OPT_OPEN_GRAPHICS,
    OPT_OPEN_INTENSIVE,
    OPT_OPEN_SYSTEM,
    OPT_PRESET_ULTRA_LOW,
    OPT_PRESET_DEFAULT,
    OPT_PRESET_ULTRA_PSP_NATIVE_UI,
    OPT_PRESET_ULTRA_PSP,
    OPT_PRESET_ULTRA_NATIVE_30,
    OPT_PRESET_CUSTOM,
    OPTION_COUNT
};

static_assert(OPT_PRESET_CUSTOM - OPT_PRESET_ULTRA_LOW + 1 == SETTING_PRESET_COUNT,
              "Each graphics preset needs a menu entry");

#define VISIBLE_OPTIONS 4

enum MenuPage {
    MENU_MAIN,
    MENU_GAMEPLAY,
    MENU_GRAPHICS,
    MENU_INTENSIVE,
    MENU_SYSTEM,
    MENU_PRESETS,
    MENU_COUNT
};

static const OptionIndex main_options[] = {
    OPT_OPEN_GAMEPLAY, OPT_OPEN_GRAPHICS, OPT_OPEN_SYSTEM
};
static const OptionIndex gameplay_options[] = {
    OPT_GAMEPLAY_SPEED, OPT_PC_RULES, OPT_ALL_CONTENT
};
static const OptionIndex graphics_options[] = {
    OPT_OPEN_PRESETS, OPT_OPEN_INTENSIVE, OPT_GHOST_EAT_OUTLINE, OPT_PACMAN_LIGHT,
    OPT_POWER_PALETTE, OPT_POWER_FLASH, OPT_POWER_PULSE, OPT_DANGER_ZOOM
};
static const OptionIndex intensive_options[] = {
    OPT_FRAME_RATE, OPT_RESOLUTION, OPT_NATIVE_UI,
    OPT_GHOST_EAT_PARTICLES, OPT_MAZE_GLOW, OPT_MSAA, OPT_MAZE_WOBBLE, OPT_IMPACT_RIPPLES,
    OPT_GHOST_AFTERIMAGES, OPT_GHOST_TRAILS, OPT_GHOST_CHAIN_TRAIL,
    OPT_GHOST_EYE_TRAILS, OPT_MOTION_BLUR_ENABLED, OPT_MOTION_BLUR
};
static const OptionIndex system_options[] = {OPT_LANGUAGE, OPT_BUILD_TYPE};
static const OptionIndex preset_options[] = {
    OPT_PRESET_ULTRA_LOW, OPT_PRESET_DEFAULT, OPT_PRESET_ULTRA_PSP_NATIVE_UI,
    OPT_PRESET_ULTRA_PSP, OPT_PRESET_ULTRA_NATIVE_30, OPT_PRESET_CUSTOM
};

struct MenuDesc {
    const char *title;
    const OptionIndex *options;
    int count;
    MenuPage parent;
};

template <size_t N>
static constexpr MenuDesc menu(const char *title, const OptionIndex (&options)[N],
                               MenuPage parent = MENU_MAIN) {
    return {title, options, (int)N, parent};
}

static const MenuDesc menus[MENU_COUNT] = {
    menu("SETTINGS", main_options),
    menu("SETTINGS / GAMEPLAY", gameplay_options),
    menu("SETTINGS / GRAPHICS", graphics_options),
    menu("SETTINGS / GRAPHICS / INTENSIVE GRAPHICS", intensive_options, MENU_GRAPHICS),
    menu("SETTINGS / SYSTEM", system_options),
    menu("SETTINGS / GRAPHICS / PRESET", preset_options, MENU_GRAPHICS)
};

static bool dirty = false;
static bool reset_holding = false;
static bool reset_fired = false;
static uint64_t reset_started = 0;
static float reset_progress = 0.0f;
static uint64_t reset_notice_until = 0;

static MenuPage current_menu = MENU_MAIN;
static int selected_row = 0;     /* menus[current_menu].count selects the buttons. */
static int selected_button = 0;  /* 0 = SAVE & EXIT, 1 = BACK/EXIT */
static int scroll_offset = 0;    /* first visible option index */
struct MenuPosition {
    int row, button, scroll;
};
static MenuPosition menu_positions[MENU_COUNT];
static vita2d_pgf *g_font = nullptr;

static void ensure_scroll_visible();

static void open_menu(MenuPage page) {
    menu_positions[current_menu] = {selected_row, selected_button, scroll_offset};
    current_menu = page;
    const MenuPosition &position = menu_positions[page];
    selected_row = position.row;
    selected_button = position.button;
    scroll_offset = position.scroll;
    if (page == MENU_PRESETS) {
        selected_row = setting_graphicsPreset;
        ensure_scroll_visible();
    }
}

static bool option_locked(int option) {
    if (setting_graphicsPreset == SETTING_PRESET_CUSTOM ||
        option == OPT_OPEN_PRESETS || option == OPT_OPEN_INTENSIVE)
        return false;
    const MenuPage graphics_pages[] = {MENU_GRAPHICS, MENU_INTENSIVE};
    for (MenuPage page : graphics_pages)
        for (int i = 0; i < menus[page].count; ++i)
            if (menus[page].options[i] == option)
                return true;
    return false;
}

#ifdef DEBUG_SOLOADER
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
#else
#define log_line(...) ((void)0)
#endif

/* Require one continuous hold, and release before another reset. */
static bool update_reset_hold(uint32_t buttons, uint64_t now) {
    const uint32_t bumpers = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    if ((buttons & bumpers) != bumpers) {
        reset_holding = false;
        reset_fired = false;
        reset_progress = 0.0f;
        return false;
    }
    if (!reset_holding) {
        reset_holding = true;
        reset_started = now;
    }
    uint64_t elapsed = now - reset_started;
    reset_progress = elapsed >= 1000000 ? 1.0f : (float)elapsed / 1000000.0f;
    if (!reset_fired && elapsed >= 1000000) {
        settings_reset();
        dirty = true;
        reset_fired = true;
        reset_notice_until = now + 3000000;
    }
    return true;
}

static void load_settings() {
    settings_load();
    log_line("load_settings: preset=%d", setting_graphicsPreset);
}

static void save_settings() {
    sceIoMkdir(CONFIG_DIR, 0777);
    const bool saved = settings_save();
    log_line("save_settings: preset=%d success=%d", setting_graphicsPreset,
             saved ? 1 : 0);
}

static bool pressed(uint32_t now, uint32_t prev, uint32_t button) {
    return (now & button) && !(prev & button);
}

static void draw_label(int x, int y, unsigned int color, float scale, const char *text) {
    if (g_font)
        vita2d_pgf_draw_text(g_font, x, y, color, scale, text);
}

static void draw_row(float x, float y, float w, const char *label, const char *value,
                     bool selected, bool locked) {
    unsigned int bg = selected ? RGBA8(242, 210, 64, 255) : RGBA8(50, 60, 76, 255);
    unsigned int fg = selected ? RGBA8(18, 18, 18, 255) : RGBA8(236, 240, 246, 255);
    unsigned int vg = selected ? RGBA8(18, 18, 18, 255) : RGBA8(216, 220, 228, 255);
    if (locked) {
        bg = selected ? RGBA8(62, 70, 84, 255) : RGBA8(30, 36, 46, 255);
        fg = vg = RGBA8(166, 174, 188, 255);
    }

    vita2d_draw_rectangle(x, y, w, 58.0f, bg);
    if (selected && locked)
        vita2d_draw_rectangle(x, y, 4.0f, 58.0f, RGBA8(242, 210, 64, 255));
    draw_label((int)x + 18, (int)y + (locked ? 26 : 38), fg, 1.0f, label);
    if (locked)
        draw_label((int)x + 18, (int)y + 47, fg, 0.6f, "LOCKED BY PRESET");
    if (value && g_font)
        draw_label((int)(x + w) - 18 - vita2d_pgf_text_width(g_font, 1.0f, value),
                   (int)y + 38, vg, 1.0f, value);
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

static const char *option_hint() {
    const MenuDesc &page = menus[current_menu];
    if (selected_row < page.count) {
        if (option_locked(page.options[selected_row]))
            return "Select Custom in Graphics > Preset to unlock these settings.";
        const int preset = page.options[selected_row] - OPT_PRESET_ULTRA_LOW;
        if (preset >= 0 && preset < SETTING_PRESET_COUNT)
            return settings_preset_description(preset);
        switch (page.options[selected_row]) {
            case OPT_OPEN_GAMEPLAY: return "Game speed, gameplay rules and content access.";
            case OPT_OPEN_GRAPHICS: return "Graphics presets, visual effects and intensive graphics options.";
            case OPT_FRAME_RATE: return "30 FPS reduces rendering load while keeping normal game speed.";
            case OPT_RESOLUTION: return "Lower resolutions look softer and reduce rendering load. Applies next launch.";
            case OPT_NATIVE_UI: return "Native menus and HUD at lower game resolutions. Adds rendering cost.";
            case OPT_OPEN_INTENSIVE: return "Frame rate, gameplay/UI resolutions, and effects that can reduce frame rate.";
            case OPT_OPEN_SYSTEM: return "Game language and release/debug selection.";
            case OPT_OPEN_PRESETS: return settings_preset_description(setting_graphicsPreset);
            case OPT_GHOST_EAT_PARTICLES: return "Blue streaks and gold sparks on ghost eats. Particle count is limited.";
            case OPT_MAZE_GLOW: return "High rendering cost. Can substantially reduce frame rate.";
            case OPT_MSAA: return "Smooths jagged edges. Higher levels can reduce frame rate.";
            case OPT_MOTION_BLUR_ENABLED:
            case OPT_MOTION_BLUR: return "PC danger blur is not restored yet; these controls do not enable it.";
            default: break;
        }
    }
    return "";
}

static void render_frame() {
    vita2d_start_drawing();
    vita2d_clear_screen();

    vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(14, 18, 28, 255));
    vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 120.0f, RGBA8(20, 34, 52, 255));
    vita2d_draw_rectangle(52.0f, 98.0f, 856.0f, 2.0f, RGBA8(242, 210, 64, 255));

    draw_label(62, 52, RGBA8(244, 246, 250, 255), 1.4f, "PAC-MAN CE DX CONFIGURATION");
    draw_label(64, 84, RGBA8(242, 210, 64, 255), 0.9f, menus[current_menu].title);
    draw_label(64, 146, RGBA8(188, 198, 218, 255), 1.0f, "D-Pad: navigate    Left/Right: change value");
    draw_label(64, 176, RGBA8(188, 198, 218, 255), 1.0f, "X: select/save    O: back/exit");
    vita2d_draw_rectangle(528.0f, 152.0f, 368.0f, 32.0f, RGBA8(50, 60, 76, 255));
    vita2d_draw_rectangle(528.0f, 182.0f, 368.0f * reset_progress, 2.0f,
                          RGBA8(242, 210, 64, 255));
    draw_label(542, 175, RGBA8(236, 240, 246, 255), 0.9f,
               reset_fired ? "DEFAULTS RESTORED" : "L + R (1s): RESET ALL");

    struct OptionDesc {
        const char *label;
        const char *value;
    };
    static const char *const languages[SETTING_LANGUAGE_COUNT + 1] = {
        "SYSTEM", "JAPANESE", "ENGLISH", "FRENCH", "ITALIAN", "GERMAN",
        "SPANISH", "RUSSIAN", "CHINESE (SIMPL.)", "KOREAN", "PORTUGUESE (BR)"
    };
    char preset_value[96];
    snprintf(preset_value, sizeof(preset_value), "%s >", settings_preset_name(setting_graphicsPreset));
    OptionDesc options[OPTION_COUNT] = {
        {"GAMEPLAY SPEED",         setting_pcSpeed ? "PC" : "ANDROID"},
        {"PC GAMEPLAY RULES",      setting_pcRules ? "ON" : "OFF"},
        {"GAME LANGUAGE",         languages[settings_sanitize_language(setting_language) + 1]},
        {"MSAA ANTI-ALIASING",    settings_msaa_to_string(setting_msaaMode)},
        {"BUILD TYPE",            setting_buildType ? "DEBUG" : "RELEASE"},
        {"PRESET",                preset_value},
        {"MOTION BLUR",            setting_motionBlur ? "ON" : "OFF"},
        {"MOTION BLUR SAMPLES",    setting_motionBlurSamples == 8 ? "8 (ORIGINAL)" :
                                  setting_motionBlurSamples == 2 ? "2 (FASTEST)" : "4 (FASTER)"},
        {"GHOST AFTERIMAGES",      setting_ghostAfterimages ? "ON" : "OFF"},
        {"GHOST AFTERIMAGE QUALITY", setting_reduceGhostTrails ? "REDUCED" : "FULL"},
        {"GHOST CHAIN TRAIL",      setting_ghostChainTrails ? "ON" : "OFF"},
        {"GHOST EYE TRAILS",       setting_ghostEyeTrails ? "ON" : "OFF"},
        {"POWERED MAZE WOBBLE",    setting_mazeWobble ? "ON" : "OFF"},
        {"GHOST-EAT OUTLINE",        setting_ghostEatOutline ? "ON" : "OFF"},
        {"PAC-MAN LIGHTING",         setting_pacmanLight ? "ON" : "OFF"},
        {"POWER-UP PALETTE",         setting_powerPalette ? "ON" : "OFF"},
        {"POWER-UP FLASHES",         setting_powerFlash ? "ON" : "OFF"},
        {"POWER-UP PULSE",           setting_powerPulse ? "ON" : "OFF"},
        {"MAZE GLOW",                setting_mazeGlow ? "ON" : "OFF"},
        {"IMPACT RIPPLES",           setting_impactRipples ? "ON" : "OFF"},
        {"DANGER ZOOM",            setting_dangerZoom ? "ON" : "OFF"},
        {"UNLOCK ALL CONTENT",   setting_unlockAllContent ? "ON" : "OFF"},
        {"GHOST-EAT PARTICLES",    setting_ghostEatParticles ? "ON" : "OFF"},
        {"FRAME RATE",            setting_frameRate == 30 ? "30 FPS" : "60 FPS"},
        {"RESOLUTION",            settings_resolution_to_string(setting_resolution)},
        {"UI RESOLUTION",         setting_nativeUi ? "NATIVE (960x544)" : "SAME AS GAME"},
        {"GAMEPLAY",              ">"},
        {"GRAPHICS",              ">"},
        {"INTENSIVE GRAPHICS",    ">"},
        {"SYSTEM",                ">"},
    };
    for (int preset = 0; preset < SETTING_PRESET_COUNT; ++preset)
        options[OPT_PRESET_ULTRA_LOW + preset] = {
            settings_preset_name(preset), setting_graphicsPreset == preset ? "SELECTED" : ""
        };

    const MenuDesc &page = menus[current_menu];
    const float list_x = 96.0f;
    const float list_w = 768.0f;
    const int base_y = 200;
    const int row_height = 58;

    for (int slot = 0; slot < VISIBLE_OPTIONS; ++slot) {
        int idx = scroll_offset + slot;
        if (idx >= page.count)
            break;
        bool selected = (selected_row == idx);
        const OptionDesc &option = options[page.options[idx]];
        draw_row(list_x, (float)(base_y + slot * row_height), list_w,
                 option.label, option.value, selected, option_locked(page.options[idx]));
    }

    unsigned int arrow_color = RGBA8(188, 198, 218, 255);
    int arrow_x = (int)(list_x + list_w + 16);
    if (scroll_offset > 0)
        draw_scroll_arrow_up(arrow_x, base_y + 14, arrow_color);
    if (scroll_offset + VISIBLE_OPTIONS < page.count)
        draw_scroll_arrow_down(arrow_x, base_y + VISIBLE_OPTIONS * row_height - 14, arrow_color);
    const char *hint = option_hint();
    float hint_scale = 0.8f;
    int hint_width = g_font ? vita2d_pgf_text_width(g_font, hint_scale, hint) : 0;
    if (hint_width > list_w)
        hint_scale *= list_w / hint_width;
    draw_label((int)list_x, 448, RGBA8(188, 198, 218, 255), hint_scale, hint);

    const float btn_y = 456.0f;
    const float btn_w = 376.0f;
    const float btn_h = 58.0f;
    const float left_x = 96.0f;
    const float right_x = 488.0f;

    bool save_selected = (selected_row == page.count && selected_button == 0);
    bool exit_selected = (selected_row == page.count && selected_button == 1);

    unsigned int save_bg = save_selected ? RGBA8(242, 210, 64, 255) : RGBA8(50, 60, 76, 255);
    unsigned int save_fg = save_selected ? RGBA8(18, 18, 18, 255) : RGBA8(236, 240, 246, 255);
    unsigned int exit_bg = exit_selected ? RGBA8(242, 210, 64, 255) : RGBA8(50, 60, 76, 255);
    unsigned int exit_fg = exit_selected ? RGBA8(18, 18, 18, 255) : RGBA8(236, 240, 246, 255);

    vita2d_draw_rectangle(left_x,  btn_y, btn_w, btn_h, save_bg);
    vita2d_draw_rectangle(right_x, btn_y, btn_w, btn_h, exit_bg);
    draw_label((int)left_x  + 110, (int)btn_y + 38, save_fg, 1.0f, "SAVE & EXIT");
    draw_label((int)right_x + 150, (int)btn_y + 38, exit_fg, 1.0f,
               current_menu == MENU_MAIN ? "EXIT" : "BACK");

    draw_label(64, 536, RGBA8(194, 200, 210, 255), 1.0f,
               sceKernelGetProcessTimeWide() < reset_notice_until
                     ? "Defaults restored. Press SAVE & EXIT to keep them."
                     : dirty ? "Unsaved changes. Press SAVE & EXIT to keep them."
                     : "Changes apply on next game launch.");

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void ensure_scroll_visible() {
    const int count = menus[current_menu].count;
    if (selected_row >= count)
        return;
    if (selected_row < scroll_offset)
        scroll_offset = selected_row;
    else if (selected_row >= scroll_offset + VISIBLE_OPTIONS)
        scroll_offset = selected_row - VISIBLE_OPTIONS + 1;
    if (scroll_offset < 0)
        scroll_offset = 0;
    int max_offset = count - VISIBLE_OPTIONS;
    if (max_offset < 0)
        max_offset = 0;
    if (scroll_offset > max_offset)
        scroll_offset = max_offset;
}

static void cycle_option(int idx, int direction) {
    if (option_locked(idx))
        return;
    reset_notice_until = 0;
    /* direction: +1 = forward/right, -1 = backward/left. */
    switch (idx) {
        case OPT_FRAME_RATE:
            setting_frameRate = setting_frameRate == 30 ? 60 : 30;
            dirty = true;
            break;
        case OPT_RESOLUTION:
            setting_resolution = (settings_sanitize_resolution(setting_resolution) +
                          (direction > 0 ? 1 : SETTING_RESOLUTION_COUNT - 1)) % SETTING_RESOLUTION_COUNT;
            dirty = true;
            break;
        case OPT_NATIVE_UI:
            setting_nativeUi = !setting_nativeUi;
            dirty = true;
            break;
        case OPT_GAMEPLAY_SPEED:
            setting_pcSpeed = setting_pcSpeed ? 0 : 1;
            dirty = true;
            break;
        case OPT_PC_RULES:
            setting_pcRules = setting_pcRules ? 0 : 1;
            dirty = true;
            break;
        case OPT_LANGUAGE:
            setting_language += direction > 0 ? 1 : -1;
            if (setting_language >= SETTING_LANGUAGE_COUNT) setting_language = SETTING_LANGUAGE_SYSTEM;
            if (setting_language < SETTING_LANGUAGE_SYSTEM) setting_language = SETTING_LANGUAGE_COUNT - 1;
            dirty = true;
            break;
        case OPT_MSAA:
            setting_msaaMode = (setting_msaaMode + (direction > 0 ? 1 : 2)) % 3;
            dirty = true;
            break;
        case OPT_BUILD_TYPE:
            setting_buildType = setting_buildType ? 0 : 1;
            dirty = true;
            break;
        case OPT_MOTION_BLUR:
            if (direction > 0)
                setting_motionBlurSamples = setting_motionBlurSamples == 2 ? 4 : setting_motionBlurSamples == 4 ? 8 : 2;
            else
                setting_motionBlurSamples = setting_motionBlurSamples == 8 ? 4 : setting_motionBlurSamples == 4 ? 2 : 8;
            dirty = true;
            break;
        case OPT_GHOST_TRAILS:
            setting_reduceGhostTrails = setting_reduceGhostTrails ? 0 : 1;
            dirty = true;
            break;
        case OPT_GHOST_CHAIN_TRAIL:
            setting_ghostChainTrails = setting_ghostChainTrails ? 0 : 1;
            dirty = true;
            break;
        case OPT_MAZE_WOBBLE:
            setting_mazeWobble = setting_mazeWobble ? 0 : 1;
            dirty = true;
            break;
        case OPT_MOTION_BLUR_ENABLED:
            setting_motionBlur = setting_motionBlur ? 0 : 1;
            dirty = true;
            break;
        case OPT_GHOST_AFTERIMAGES:
            setting_ghostAfterimages = setting_ghostAfterimages ? 0 : 1;
            dirty = true;
            break;
        case OPT_GHOST_EAT_OUTLINE:
            setting_ghostEatOutline = setting_ghostEatOutline ? 0 : 1;
            dirty = true;
            break;
        case OPT_PACMAN_LIGHT:
            setting_pacmanLight = setting_pacmanLight ? 0 : 1;
            dirty = true;
            break;
        case OPT_POWER_PALETTE:
            setting_powerPalette = setting_powerPalette ? 0 : 1;
            dirty = true;
            break;
        case OPT_POWER_FLASH:
            setting_powerFlash = setting_powerFlash ? 0 : 1;
            dirty = true;
            break;
        case OPT_POWER_PULSE:
            setting_powerPulse = setting_powerPulse ? 0 : 1;
            dirty = true;
            break;
        case OPT_MAZE_GLOW:
            setting_mazeGlow = setting_mazeGlow ? 0 : 1;
            dirty = true;
            break;
        case OPT_IMPACT_RIPPLES:
            setting_impactRipples = setting_impactRipples ? 0 : 1;
            dirty = true;
            break;
        case OPT_DANGER_ZOOM:
            setting_dangerZoom = setting_dangerZoom ? 0 : 1;
            dirty = true;
            break;
        case OPT_GHOST_EYE_TRAILS:
            setting_ghostEyeTrails = setting_ghostEyeTrails ? 0 : 1;
            dirty = true;
            break;
        case OPT_GHOST_EAT_PARTICLES:
            setting_ghostEatParticles = setting_ghostEatParticles ? 0 : 1;
            dirty = true;
            break;
        case OPT_ALL_CONTENT:
            setting_unlockAllContent = setting_unlockAllContent ? 0 : 1;
            dirty = true;
            break;
        default:
            break;
    }
}

static void activate_option(int direction) {
    const OptionIndex option = menus[current_menu].options[selected_row];
    switch (option) {
        case OPT_OPEN_PRESETS: if (direction > 0) open_menu(MENU_PRESETS); break;
        case OPT_OPEN_GAMEPLAY: if (direction > 0) open_menu(MENU_GAMEPLAY); break;
        case OPT_OPEN_GRAPHICS: if (direction > 0) open_menu(MENU_GRAPHICS); break;
        case OPT_OPEN_INTENSIVE: if (direction > 0) open_menu(MENU_INTENSIVE); break;
        case OPT_OPEN_SYSTEM: if (direction > 0) open_menu(MENU_SYSTEM); break;
        default:
            if (option >= OPT_PRESET_ULTRA_LOW && option <= OPT_PRESET_CUSTOM) {
                const int preset = option - OPT_PRESET_ULTRA_LOW;
                if (direction > 0 && preset != setting_graphicsPreset) {
                    settings_apply_graphics_preset(preset);
                    dirty = true;
                    reset_notice_until = 0;
                }
            } else {
                cycle_option(option, direction);
            }
            break;
    }
}

static void handle_controls(uint32_t buttons, uint32_t prev_buttons,
                            bool &running, bool &save_on_exit) {
    const int count = menus[current_menu].count;
    /* Handle one action per press so entering a page cannot also edit its first row. */
    if (pressed(buttons, prev_buttons, SCE_CTRL_CIRCLE)) {
        if (current_menu == MENU_MAIN) {
            save_on_exit = false;
            running = false;
        } else {
            open_menu(menus[current_menu].parent);
        }
    } else if (pressed(buttons, prev_buttons, SCE_CTRL_UP)) {
        if (selected_row > 0)
            --selected_row;
        ensure_scroll_visible();
    } else if (pressed(buttons, prev_buttons, SCE_CTRL_DOWN)) {
        if (selected_row < count)
            ++selected_row;
        ensure_scroll_visible();
    } else if (pressed(buttons, prev_buttons, SCE_CTRL_CROSS)) {
        if (selected_row != count) {
            activate_option(+1);
        } else if (selected_button == 0) {
            save_on_exit = true;
            running = false;
        } else if (current_menu != MENU_MAIN) {
            open_menu(menus[current_menu].parent);
        } else {
            save_on_exit = false;
            running = false;
        }
    } else if (pressed(buttons, prev_buttons, SCE_CTRL_LEFT)) {
        if (selected_row == count)
            selected_button = 0;
        else
            activate_option(-1);
    } else if (pressed(buttons, prev_buttons, SCE_CTRL_RIGHT)) {
        if (selected_row == count)
            selected_button = 1;
        else
            activate_option(+1);
    }
}

int main() {
#ifdef DEBUG_SOLOADER
    sceIoRemove(LOG_FILE);
#endif
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
        if (sceCtrlPeekBufferPositive(0, &pad, 1) <= 0)
            pad.buttons = 0;
        uint32_t buttons = pad.buttons;
        if (update_reset_hold(buttons, sceKernelGetProcessTimeWide()))
            buttons = 0; /* Keep other controls from editing or saving mid-hold. */

        handle_controls(buttons, prev_buttons, running, save_on_exit);

        render_frame();
        prev_buttons = pad.buttons;
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
