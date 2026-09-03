#include "game_map_effects.h"
#include "game_patch.h"
#include "settings.h"
#include "game_frame_rate.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static const unsigned char *effect_pacman;
static const unsigned char *effect_ghosts, *ghost_light_tune;
static int (*ghost_is_dark)(void);
static int (*effect_is_game)(void), (*effect_is_preview)(void), (*effect_is_paused)(void);
static so_hook pellet_hook;
static float player[4], power[4], flash;
static int snapshot_dirty = 1;

enum { MAP_EFFECT_PROGRAMS = 16, GHOST_RECORDS = 64, GHOST_LIGHTS = 4 };
/* Two vec4s per light: screen position/radius/fade, then RGB. No task pointers
 * or additional textures are retained by the lighting pass. */
static float ghost_lights[GHOST_LIGHTS * 2][4], ghost_fade[GHOST_RECORDS];
static unsigned ghost_selected[GHOST_LIGHTS]; /* Record index + 1; zero is unused. */
static struct {
    GLuint program;
    GLint player, power, ghosts;
} programs[MAP_EFFECT_PROGRAMS];

static int enabled(void) {
    return !setting_lowPerformance && (setting_pacmanLight || setting_powerPalette ||
        setting_powerFlash || setting_powerPulse || setting_mazeGlow || setting_ghostLights);
}

static float unit(float v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static float number(const void *object, unsigned offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static void rotate_hue(float *rgb, float phase) {
    if (!isfinite(phase) || fabsf(phase) > 10000.0f)
        return;
    float hi = rgb[0], lo = rgb[0];
    for (unsigned i = 1; i < 3; ++i) {
        if (rgb[i] > hi) hi = rgb[i];
        if (rgb[i] < lo) lo = rgb[i];
    }
    float range = hi - lo;
    if (range <= 0.0f)
        return;
    float h = hi == rgb[0] ? (rgb[1] - rgb[2]) / range :
        hi == rgb[1] ? (rgb[2] - rgb[0]) / range + 2.0f :
                      (rgb[0] - rgb[1]) / range + 4.0f;
    h = h / 6.0f + phase;
    h -= (int)h;
    if (h < 0.0f) h += 1.0f;
    h *= 6.0f;
    unsigned sector = (unsigned)h;
    float rise = lo + range * (h - sector), fall = hi - range * (h - sector);
    const float colors[6][3] = {
        {hi, rise, lo}, {fall, hi, lo}, {lo, hi, rise},
        {lo, fall, hi}, {rise, lo, hi}, {hi, lo, fall}
    };
    memcpy(rgb, colors[sector % 6], 3 * sizeof(float));
}

static void update_ghost_lights(float px, float py, int paused) {
    if (!setting_ghostLights || !effect_ghosts)
        return;
    static const unsigned tune_index[] = {3, 4, 5, 6, 8, 9, 10};
    static const float fallback[][3] = {
        {1.0f, .15f, .1f}, {1.0f, .4f, .65f}, {.1f, .85f, 1.0f},
        {1.0f, .5f, .1f}, {.85f, .85f, .85f}, {1.0f, .15f, .1f}, {.2f, .3f, 1.0f}
    };
    float nearest[GHOST_LIGHTS] = {1e20f, 1e20f, 1e20f, 1e20f};
    unsigned selected[GHOST_LIGHTS] = {0};
    unsigned bank = ghost_is_dark() ? 240 : 0;
    for (unsigned i = 0; i < GHOST_RECORDS; ++i) {
        const unsigned char *g = effect_ghosts + i * 48;
        int16_t kind;
        memcpy(&kind, g + 28, sizeof(kind));
        float x = number(g, 0), y = number(g, 4);
        if (!(g[30] & 1) || (g[31] & 1) || kind < 0 || kind > 6 ||
            !isfinite(x) || !isfinite(y) || x < -120 || x > 1400 || y < -120 || y > 840) {
            ghost_fade[i] = 0.0f;
            continue;
        }
        int frightened = g[32] & 1;
        const unsigned char *tune = ghost_light_tune + bank + 20 * (frightened ? 2 : tune_index[kind]);
        float color[4] = {number(tune, 0), number(tune, 4), number(tune, 8), number(tune, 12)};
        float scale = number(tune, 16);
        /* Some mobile tuning sets leave the unused light entries empty. */
        if (!isfinite(scale) || scale <= 0 || !isfinite(color[0]) ||
            !isfinite(color[1]) || !isfinite(color[2]) || !isfinite(color[3])) {
            memcpy(color, fallback[frightened ? 6 : kind], 3 * sizeof(float));
            color[3] = 1.0f;
            scale = 1.2f;
        }
        for (unsigned c = 0; c < 4; ++c) color[c] = unit(color[c]);
        if (kind == 5 && !frightened) rotate_hue(color, number(g, 44));
        if (!paused) ghost_fade[i] = unit(ghost_fade[i] + .12f * game_frame_rate_render_ticks());
        float radius = 80.0f * scale;
        radius = radius < 48.0f ? 48.0f : radius > 120.0f ? 120.0f : radius;
        float dx = x - px, dy = y - py, distance = dx * dx + dy * dy;
        /* Keep a selected light until a replacement is appreciably closer. */
        for (unsigned j = 0; j < GHOST_LIGHTS; ++j)
            if (ghost_selected[j] == i + 1) distance *= .85f;
        if (color[3] <= 0.0f)
            continue;
        unsigned slot = 0;
        while (slot < GHOST_LIGHTS && distance >= nearest[slot]) ++slot;
        if (slot == GHOST_LIGHTS)
            continue;
        for (unsigned j = GHOST_LIGHTS - 1; j > slot; --j) {
            nearest[j] = nearest[j - 1];
            selected[j] = selected[j - 1];
            memcpy(ghost_lights[j * 2], ghost_lights[(j - 1) * 2], 8 * sizeof(float));
        }
        nearest[slot] = distance;
        selected[slot] = i + 1;
        float *light = ghost_lights[slot * 2];
        light[0] = x / 1280.0f;
        light[1] = y / 720.0f;
        light[2] = (720.0f / radius) * (720.0f / radius);
        light[3] = color[3] * ghost_fade[i];
        memcpy(ghost_lights[slot * 2 + 1], color, sizeof(color));
    }
    memcpy(ghost_selected, selected, sizeof(selected));
}

void game_map_effects_power_pellet(void) {
    if (setting_powerFlash) {
        flash = 1.0f;
        snapshot_dirty = 1;
    }
}

static void power_pellet(void *pacman, int sound) {
    so_hook_unpatch(&pellet_hook);
    ((void (*)(void *, int))pellet_hook.thumb_addr)(pacman, sound);
    so_hook_repatch(&pellet_hook);
    game_map_effects_power_pellet();
}

void game_map_effects_install_hooks(void) {
    if (!enabled() || !game_patch_checked_function(
            "_ZN9newPacman13cOnPacmanTask4FuncEv", 0xef0, 0x00ae5623u))
        return;
    effect_is_game = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence6IsGameEv", 0x14, 0x476f8a74u);
    effect_is_preview = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence9IsPreviewEv", 0x14, 0x1128e4ffu);
    effect_is_paused = (void *)game_patch_checked_function(
        "_ZN9newPacman12GetPauseFlagEv", 0x10, 0xd3471ef9u);
    if (!effect_is_game || !effect_is_preview || !effect_is_paused)
        return;
    effect_pacman = (void *)so_symbol(&so_mod,
        "_ZN9newPacman17CStaticEffectInfo6pacmanE");
    if (effect_pacman && setting_ghostLights &&
        game_patch_checked_function("_ZN9newPacman21cTsTaskCharacterLight4FuncEv", 0x470, 0xd88aec42u) &&
        game_patch_checked_function("_ZN9newPacman12cOnGhostTask4FuncEv", 0x934, 0x15535a5eu) &&
        game_patch_checked_function("_ZN9newPacman17CStaticEffectInfo24ReleaseGhostEffectVectorEPNS_13tEffectVectorE", 0x18, 0xbb6b48c0u) &&
        game_patch_checked_function("_ZN9newPacman13tEffectVector6SetPosEff", 0x46, 0x1076847cu)) {
        ghost_is_dark = (void *)game_patch_checked_function(
            "_ZN9newPacman11CPacmanGame14IsKurayamiModeEv", 0x14, 0xf9bce6acu);
        ghost_light_tune = (void *)so_symbol(&so_mod, "_ZN3sys8cPacTune14CharacterLightE");
        if (ghost_is_dark && ghost_light_tune)
            effect_ghosts = (void *)so_symbol(&so_mod, "_ZN9newPacman17CStaticEffectInfo5ghostE");
    }
    if (effect_pacman && setting_powerFlash) {
        uintptr_t pellet = game_patch_checked_function(
            "_ZN9newPacman13cOnPacmanTask8OnEatPwoEb", 0x47c, 0x9a442bf7u);
        if (pellet)
            pellet_hook = hook_addr(pellet, (uintptr_t)power_pellet);
    }
}

void game_map_effects_next_frame(void) {
    snapshot_dirty = 1;
}

static void update(void) {
    if (!snapshot_dirty)
        return;
    snapshot_dirty = 0;
    memset(player, 0, sizeof(player));
    memset(power, 0, sizeof(power));
    memset(ghost_lights, 0, sizeof(ghost_lights));
    if (!enabled() || !effect_pacman || !effect_is_game() || effect_is_preview() ||
        !(effect_pacman[30] & 1)) {
        flash = 0.0f;
        memset(ghost_fade, 0, sizeof(ghost_fade));
        memset(ghost_selected, 0, sizeof(ghost_selected));
        return;
    }
    float x = number(effect_pacman, 0), y = number(effect_pacman, 4);
    float fade = number(effect_pacman, 36);
    if (!isfinite(x) || !isfinite(y) || !isfinite(fade)) {
        flash = 0.0f;
        return;
    }
    player[0] = unit(x / 1280.0f);
    player[1] = unit(y / 720.0f);
    player[2] = 1.0f;
    player[3] = (effect_pacman[33] & 1) ? 1.0f : 0.0f;
    power[0] = player[3] * unit(fade);
    power[1] = flash;
    /* The checked update stores min(remaining power timer / 120, 1).
     * Four smooth pulses in that final interval remain tied to native time. */
    float phase = power[0] * 4.0f;
    phase -= (int)phase;
    float triangle = 1.0f - fabsf(phase * 2.0f - 1.0f);
    power[2] = player[3] * (1.0f - power[0]) * triangle * triangle * (3.0f - 2.0f * triangle);
    power[3] = 1.0f;
    int paused = effect_is_paused();
    update_ghost_lights(x, y, paused);
    if (!paused) {
        for (int i = 0; i < game_frame_rate_render_ticks(); ++i)
            flash *= 0.84f;
        if (flash < 0.002f)
            flash = 0.0f;
    }
}

void game_map_effects_register(GLuint program) {
    unsigned slot;
    for (slot = 0; slot < MAP_EFFECT_PROGRAMS; ++slot)
        if (programs[slot].program == program)
            break;
    if (slot == MAP_EFFECT_PROGRAMS)
        for (slot = 0; slot < MAP_EFFECT_PROGRAMS; ++slot)
            if (!programs[slot].program)
                break;
    if (!program || slot == MAP_EFFECT_PROGRAMS)
        return;
    programs[slot].program = program;
    programs[slot].player = glGetUniformLocation(program, "u_pmcMapPlayer");
    programs[slot].power = glGetUniformLocation(program, "u_pmcMapPower");
    programs[slot].ghosts = setting_ghostLights ? glGetUniformLocation(program, "u_pmcGhost") : -1;
}

void game_map_effects_invalidate(GLuint program) {
    for (unsigned slot = 0; slot < MAP_EFFECT_PROGRAMS; ++slot)
        if (programs[slot].program == program)
            programs[slot].program = 0;
}

int game_map_effects_uniform_count(GLuint program) {
    for (unsigned slot = 0; program && slot < MAP_EFFECT_PROGRAMS; ++slot)
        if (programs[slot].program == program)
            return (programs[slot].player != -1) + (programs[slot].power != -1) + (programs[slot].ghosts != -1);
    return 0;
}

void game_map_effects_apply(GLuint program) {
    if (!program || !enabled())
        return;
    unsigned slot;
    for (slot = 0; slot < MAP_EFFECT_PROGRAMS; ++slot)
        if (programs[slot].program == program)
            break;
    if (slot == MAP_EFFECT_PROGRAMS)
        return;
    update();
    if (programs[slot].player != -1)
        glUniform4fv(programs[slot].player, 1, player);
    if (programs[slot].power != -1)
        glUniform4fv(programs[slot].power, 1, power);
    if (programs[slot].ghosts != -1)
        glUniform4fv(programs[slot].ghosts, GHOST_LIGHTS * 2, &ghost_lights[0][0]);
}

static char *replace(const char *source, const char *from, const char *to) {
    const char *match = strstr(source, from);
    if (!match)
        return NULL;
    size_t before = match - source, removed = strlen(from), inserted = strlen(to);
    size_t length = strlen(source);
    if (inserted > SIZE_MAX - (length - removed) - 1)
        return NULL;
    char *result = malloc(length - removed + inserted + 1);
    if (result) {
        memcpy(result, source, before);
        memcpy(result + before, to, inserted);
        memcpy(result + before + inserted, match + removed, length - before - removed + 1);
    }
    return result;
}

static const char glow_blend[] =
    "vec3 pmcGlow;\n"
    "if(v_oColor.a == 0.0) pmcGlow = pmcGlowPrevious(v_oTexCoord2);\n"
    "else {\n"
    "    pmcGlow = pmcGlowCurrent(v_oTexCoord);\n"
    "    if(v_oColor.a < 1.0) pmcGlow = mix(pmcGlowPrevious(v_oTexCoord2), pmcGlow, v_oColor.a);\n"
    "}\n";

char *game_map_effects_single_shader(const char *source) {
    if (!source || !strstr(source, glow_blend))
        return NULL;
    return replace(source, glow_blend, "vec3 pmcGlow = pmcGlowCurrent(v_oTexCoord);\n");
}

char *game_map_effects_shader(const char *source) {
    if (!source || !enabled() || strstr(source, "PMC_MAP_EFFECTS") ||
        !strstr(source, "varying vec4 v_oPosition;") ||
        !strstr(source, "varying vec2 v_oTexCoord2;") ||
        !strstr(source, "gl_FragColor = oColor;") ||
        !strstr(source, "float maska = base.a;") ||
        !strstr(source, "uniform sampler2D u_diffuseMap2;"))
        return NULL;

    /* Existing clip-position varying avoids activating a_sprScale or changing
     * any maze vertex input. Both source variants keep their projection. */
    char declarations[3072] =
        "// PMC_MAP_EFFECTS\n"
        "uniform vec4 u_pmcMapPlayer;\n"
        "uniform vec4 u_pmcMapPower;\n";
    if (setting_ghostLights)
        strcat(declarations,
            "uniform vec4 u_pmcGhost[8];\n"
            "vec3 pmcGhostLight(vec2 p, vec4 light, vec4 color) {\n"
            "vec2 d = (p-light.xy) * vec2(1.7777778,1.0);\n"
            "float f = max(1.0-dot(d,d)*light.z,0.0);\n"
            "return color.rgb * f*f * light.w;\n}\n");
    if (setting_mazeGlow) {
        strcat(declarations,
            "vec3 pmcGlowCurrent(vec2 uv) {\n"
            "return 0.25 * (texture2D(u_diffuseMap, clamp(uv+vec2(0.0075,0.0),0.0,1.0)).rgb +\n"
            "texture2D(u_diffuseMap, clamp(uv-vec2(0.0075,0.0),0.0,1.0)).rgb +\n"
            "texture2D(u_diffuseMap, clamp(uv+vec2(0.0,0.0075),0.0,1.0)).rgb +\n"
            "texture2D(u_diffuseMap, clamp(uv-vec2(0.0,0.0075),0.0,1.0)).rgb);\n}\n"
            "vec3 pmcGlowPrevious(vec2 uv) {\n"
            "return 0.25 * (texture2D(u_diffuseMap2, clamp(uv+vec2(0.0075,0.0),0.0,1.0)).rgb +\n"
            "texture2D(u_diffuseMap2, clamp(uv-vec2(0.0075,0.0),0.0,1.0)).rgb +\n"
            "texture2D(u_diffuseMap2, clamp(uv+vec2(0.0,0.0075),0.0,1.0)).rgb +\n"
            "texture2D(u_diffuseMap2, clamp(uv-vec2(0.0,0.0075),0.0,1.0)).rgb);\n}\n");
    }
    strcat(declarations, "void main ()");
    char *result = replace(source, "void main ()", declarations);
    if (!result)
        return NULL;

    char effect[3072] = "if(u_pmcMapPower.w > 0.0) {\n";
    if (setting_powerPalette)
        strcat(effect,
            "float pmcLuma = dot(base.rgb * voColor.rgb, vec3(0.299,0.587,0.114));\n"
            "vec3 pmcTint = pmcLuma * vec3(1.4,0.52,0.92);\n"
            "oColor.rgb = mix(oColor.rgb, pmcTint, 0.3 * u_pmcMapPower.x);\n");
    if (setting_pacmanLight)
        strcat(effect,
            "vec2 pmcPosition = v_oPosition.xy * vec2(0.5,-0.5) + vec2(0.5);\n"
            "vec2 pmcDistance = (pmcPosition - u_pmcMapPlayer.xy) * vec2(1.7777778,1.0);\n"
            "float pmcLight = max(1.0 - dot(pmcDistance,pmcDistance) * 16.0, 0.0);\n"
            "pmcLight *= pmcLight * u_pmcMapPlayer.z;\n"
            "vec3 pmcLightColor = mix(vec3(1.0,0.8,0.3),vec3(1.0,0.35,0.6),u_pmcMapPlayer.w);\n"
            "oColor.rgb += pmcLightColor * pmcLight * (0.08 + 0.1 * (1.0-maska));\n");
    if (setting_mazeGlow) {
        strcat(effect, glow_blend);
        strcat(effect, "oColor.rgb += pmcGlow * voColor.rgb * 0.18;\n");
    }
    if (setting_ghostLights)
        strcat(effect,
            "vec2 pmcGhostPosition = v_oPosition.xy * vec2(0.5,-0.5) + vec2(0.5);\n"
            "vec3 pmcGhostColor = pmcGhostLight(pmcGhostPosition,u_pmcGhost[0],u_pmcGhost[1])\n"
            "+ pmcGhostLight(pmcGhostPosition,u_pmcGhost[2],u_pmcGhost[3])\n"
            "+ pmcGhostLight(pmcGhostPosition,u_pmcGhost[4],u_pmcGhost[5])\n"
            "+ pmcGhostLight(pmcGhostPosition,u_pmcGhost[6],u_pmcGhost[7]);\n"
            "oColor.rgb += min(pmcGhostColor,vec3(1.5)) * (0.04 + 0.2*(1.0-maska));\n");
    if (setting_powerFlash)
        strcat(effect, "oColor.rgb += vec3(0.16) * u_pmcMapPower.y;\n");
    if (setting_powerPulse)
        strcat(effect, "oColor.rgb *= 1.0 + 0.14 * u_pmcMapPower.z;\n");
    strcat(effect, "oColor.rgb = min(oColor.rgb,vec3(1.0));\n}\ngl_FragColor = oColor;");
    char *patched = replace(result, "gl_FragColor = oColor;", effect);
    free(result);
    return patched;
}
