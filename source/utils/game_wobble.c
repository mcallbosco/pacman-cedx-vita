#include "game_wobble.h"
#include "game_patch.h"
#include "game_shader.h"
#include "settings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

/* The loader uses Android softfp; the SDK's libm uses hard-float arguments. */
extern float wobble_sine(float) __asm__("sinf") __attribute__((pcs("aapcs-vfp")));

/* The inspected PC map uses 36 vertical cells. Sampling its short wave on
 * that grid gives a gentler shape than densely tessellating the sine itself.
 * Keep the native quad layout to use vitaGL's shared client-array upload. */
enum { WOBBLE_COLUMNS = 64, WOBBLE_ROWS = 36, WOBBLE_WORDS = 10,
       WOBBLE_VERTICES = WOBBLE_COLUMNS * WOBBLE_ROWS * 4,
       RIPPLE_CAPACITY = 2, RIPPLE_LIFE = 32 };

static so_hook horizontal_hook;
static const unsigned char *wobble_pacman;
static int (*wobble_is_game)(void);
static int (*wobble_is_preview)(void);
static float wobble_amplitude, wobble_phase, wobble_wavelength;
static float *wobble_vertices;
static int wobble_attempted;
static int (*wobble_is_paused)(void);
static so_hook ripple_event_hook, ripple_loop_hook;
static uintptr_t ripple_loop_address;
typedef struct { float x, y, strength; unsigned age; } ImpactRipple;
static ImpactRipple ripples[RIPPLE_CAPACITY];
static unsigned ripple_count, ripple_version, ripple_grid_version;
static float ripple_grid[WOBBLE_ROWS + 1][WOBBLE_COLUMNS + 1][2];

static float number(const void *object, unsigned offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static unsigned word(const void *object, unsigned offset) {
    unsigned value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static int ripple_scene(void) {
    return setting_impactRipples && ripple_loop_hook.thumb_addr && wobble_pacman && wobble_is_game() &&
        !wobble_is_preview() && !wobble_is_paused() && (wobble_pacman[30] & 1);
}

static void ripple_event(void *event) {
    /* Native events begin with two ticks of life. Read the initialized record
     * on its first update, before the original method decrements or frees it. */
    unsigned type = word(event, 88);
    if (word(event, 84) == 2 && (type == 0 || type == 2 || type == 9) &&
        ripple_scene()) {
        float x = number(event, 28), y = number(event, 32);
        if (isfinite(x) && isfinite(y) && x >= 0.0f && x <= 1280.0f &&
            y >= 0.0f && y <= 720.0f) {
            /* Dense ghost chains have a fixed cost and never retain task
             * pointers. At capacity the oldest ring yields to the new hit. */
            unsigned slot = ripple_count;
            if (slot == RIPPLE_CAPACITY)
                slot = ripples[0].age >= ripples[1].age ? 0 : 1;
            else
                ++ripple_count;
            ripples[slot] = (ImpactRipple){x, y, type == 9 ? 4.0f : 2.5f, 0};
            ++ripple_version;
        }
    }
    so_hook_unpatch(&ripple_event_hook);
    ((void (*)(void *))ripple_event_hook.thumb_addr)(event);
    so_hook_repatch(&ripple_event_hook);
}

static void ripple_loop(void) {
    if (!ripple_scene()) {
        ripple_count = 0;
    } else {
        for (unsigned i = 0; i < ripple_count;) {
            if (++ripples[i].age >= RIPPLE_LIFE)
                ripples[i] = ripples[--ripple_count];
            else
                ++i;
        }
    }
    ++ripple_version;
    so_hook_unpatch(&ripple_loop_hook);
    ((void (*)(void))ripple_loop_hook.thumb_addr)();
    so_hook_repatch(&ripple_loop_hook);
}

static void install_ripples(void) {
    if (!setting_impactRipples)
        return;
    uintptr_t event = game_patch_checked_function(
        "_ZN9newPacman20cEffectEvtVectorTask4FuncEv", 0x2a, 0xc3bc256cu);
    uintptr_t loop = game_patch_checked_function(
        "_ZN9newPacman8LoopFuncEv", 0xc4, 0xdf9819cau);
    wobble_is_paused = (void *)game_patch_checked_function(
        "_ZN9newPacman12GetPauseFlagEv", 0x10, 0xd3471ef9u);
    if (!event || !loop || !wobble_is_paused ||
        !game_patch_checked_function("_ZN9newPacman20cEffectEvtVectorTaskC2Ei",
            0x68, 0x9fbc5938u) ||
        !game_patch_checked_function("_ZN9newPacman20cEffectEvtVectorTask7SetTypeEi",
            0x18, 0xe4c62291u) ||
        !game_patch_checked_function("_ZN9newPacman13tEffectVector6SetPosEff",
            0x46, 0x1076847cu) ||
        !game_patch_checked_function("_ZN9newPacman13cOnPacmanTask18TestPacmanEatGhostEv",
            0x734, 0x05ace6f0u) ||
        !game_patch_checked_function("_ZN9newPacman13cOnPacmanTask8OnEatPwoEb",
            0x47c, 0x9a442bf7u) ||
        !game_patch_checked_function("_ZN9newPacman13cOnPacmanTask12OnPacmanBombEv",
            0x634, 0x4c85fa76u))
        return;
    ripple_event_hook = hook_addr(event, (uintptr_t)ripple_event);
    ripple_loop_address = loop;
}

void game_wobble_install_late_hooks(void) {
    /* The eye-trail installer also checks LoopFunc. Attach only after it has
     * completed, and reject an intervening change to that shared caller. */
    if (ripple_loop_address && game_patch_checked_code(ripple_loop_address,
            "impact ripple update", 0xc4, 0xdf9819cau))
        ripple_loop_hook = hook_addr(ripple_loop_address, (uintptr_t)ripple_loop);
}

static void horizontal_wave(float amplitude, float period, float wavelength) {
    /* Use the typed softfp call: an unprototyped call would promote floats. */
    so_hook_unpatch(&horizontal_hook);
    ((void (*)(float, float, float))horizontal_hook.thumb_addr)(amplitude, period, wavelength);
    so_hook_repatch(&horizontal_hook);
    if (!isfinite(amplitude) || !isfinite(period) || !isfinite(wavelength) ||
        amplitude < 1.0f || period < 1.0f || wavelength < 30.0f) {
        wobble_amplitude = 0.0f;
        return;
    }
    wobble_amplitude = amplitude > 20.0f ? 20.0f : amplitude;
    wobble_wavelength = wavelength;
    wobble_phase += 1.0f / period;
    if (wobble_phase >= 1.0f)
        wobble_phase -= 1.0f;
}

void game_wobble_install_hooks(void) {
    if ((!setting_mazeWobble && !setting_impactRipples) || setting_lowPerformance)
        return;
    uintptr_t wave = game_patch_checked_function(
        "_ZN3sys15MapWaveShEffect10SetHorWaveEfff", 0x5c, 0x4f98f9f3u);
    if (!wave || !game_patch_checked_function(
            "_ZN9newPacman13cOnPacmanTask4FuncEv", 0xef0, 0x00ae5623u))
        return;
    wobble_is_game = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence6IsGameEv", 0x14, 0x476f8a74u);
    wobble_is_preview = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence9IsPreviewEv", 0x14, 0x1128e4ffu);
    if (!wobble_is_game || !wobble_is_preview)
        return;
    wobble_pacman = (void *)so_symbol(&so_mod,
        "_ZN9newPacman17CStaticEffectInfo6pacmanE");
    if (wobble_pacman) {
        if (setting_mazeWobble)
            horizontal_hook = hook_addr(wave, (uintptr_t)horizontal_wave);
        install_ripples();
    }
}

void game_wobble_prepare(void) {
    if ((!setting_mazeWobble && !ripple_loop_address) ||
        !wobble_pacman || wobble_attempted)
        return;
    wobble_attempted = 1;
    /* One spare record also bounds the driver's per-attribute fallback,
     * which copies a full stride starting at each attribute's offset. */
    wobble_vertices = malloc((WOBBLE_VERTICES + 1) * WOBBLE_WORDS * sizeof(float));
}

/* A rectangular column has vertically constant colors/fade weights and
 * matching UV x values. Other geometry keeps the existing draw unchanged. */
static int rectangular_columns(const float *vertices, unsigned count) {
    for (unsigned cell = 0; cell < count / 4; ++cell) {
        const float *tl = vertices + cell * 40, *tr = tl + 10;
        const float *br = tl + 20, *bl = tl + 30;
        for (unsigned i = 0; i < 40; ++i)
            if (!isfinite(tl[i])) return 0;
        for (unsigned corner = 0; corner < 4; ++corner)
            if (fabsf(tl[corner * 10]) > 16384.0f ||
                fabsf(tl[corner * 10 + 1]) > 16384.0f) return 0;
        if (tl[0] != bl[0] || tr[0] != br[0] ||
            tl[1] != tr[1] || bl[1] != br[1] || tl[1] >= bl[1] ||
            tl[0] >= tr[0] || tl[1] != vertices[1] || bl[1] != vertices[31])
            return 0;
        for (unsigned i = 2; i <= 4; i += 2)
            if (tl[i] != bl[i] || tr[i] != br[i] ||
                tl[i + 1] != tr[i + 1] || bl[i + 1] != br[i + 1])
                return 0;
        if (memcmp(tl + 6, bl + 6, 16) || memcmp(tr + 6, br + 6, 16))
            return 0;
    }
    return 1;
}

static void prepare_ripple_grid(void) {
    if (ripple_grid_version == ripple_version)
        return;
    /* Two compact radial pulses on one shared logical grid. Squared-distance
     * rejection avoids square roots outside the moving annulus; the smooth
     * polynomial needs no per-vertex trigonometry or new rendering pass. */
    for (unsigned row = 0; row <= WOBBLE_ROWS; ++row) {
        for (unsigned col = 0; col <= WOBBLE_COLUMNS; ++col) {
            float x = col * 20.0f, y = row * 20.0f;
            float ox = 0.0f, oy = 0.0f;
            for (unsigned i = 0; i < ripple_count; ++i) {
                const ImpactRipple *wave = &ripples[i];
                float radius = 24.0f + wave->age * 13.0f;
                float dx = x - wave->x, dy = y - wave->y;
                float distance2 = dx * dx + dy * dy;
                float inner = radius > 64.0f ? radius - 64.0f : 0.0f;
                float outer = radius + 64.0f;
                if (distance2 <= inner * inner || distance2 >= outer * outer ||
                    distance2 < 0.0001f)
                    continue;
                float distance = sqrtf(distance2);
                float q = (distance - radius) / 64.0f;
                float envelope = 1.0f - q * q;
                float fade = 1.0f - (float)wave->age / RIPPLE_LIFE;
                float displacement = 3.493856f * wave->strength * fade *
                    q * envelope * envelope / distance;
                ox += dx * displacement;
                oy += dy * displacement;
            }
            ripple_grid[row][col][0] = ox;
            ripple_grid[row][col][1] = oy;
        }
    }
    ripple_grid_version = ripple_version;
}

static void ripple_displacement(float x, float y, float *dx, float *dy) {
    x = x < 0.0f ? 0.0f : x > 1280.0f ? 1280.0f : x;
    y = y < 0.0f ? 0.0f : y > 720.0f ? 720.0f : y;
    float gx = x / 20.0f, gy = y / 20.0f;
    unsigned col = (unsigned)gx, row = (unsigned)gy;
    if (col == WOBBLE_COLUMNS) --col;
    if (row == WOBBLE_ROWS) --row;
    float tx = gx - col, ty = gy - row;
    float value[2];
    for (unsigned i = 0; i < 2; ++i) {
        float a = ripple_grid[row][col][i];
        float b = ripple_grid[row][col + 1][i];
        float c = ripple_grid[row + 1][col][i];
        float d = ripple_grid[row + 1][col + 1][i];
        value[i] = (a + (b - a) * tx) * (1.0f - ty) + (c + (d - c) * tx) * ty;
    }
    *dx = value[0];
    *dy = value[1];
}

static void build_wobble_mesh(const float *vertices, unsigned count,
                             float width, float height) {
    /* Cache the shared row displacement across all layers of the same frame.
     * Only 37 sines are evaluated, rather than one for every maze vertex. */
    static float row_wave[WOBBLE_ROWS + 1];
    static float old_phase = -1.0f, old_amplitude, old_length, old_height;
    static float old_top, old_bottom;
    float top = vertices[1], bottom = vertices[31];
    float amplitude = setting_mazeWobble ? wobble_amplitude : 0.0f;
    if (old_phase != wobble_phase || old_amplitude != amplitude ||
        old_length != wobble_wavelength || old_height != height ||
        old_top != top || old_bottom != bottom) {
        for (unsigned row = 0; row <= WOBBLE_ROWS; ++row) {
            float y = top + (bottom - top) * ((float)row / WOBBLE_ROWS);
            row_wave[row] = amplitude >= 1.0f ?
                (0.35f * amplitude) * wobble_sine(6.28318530718f *
                (y * (720.0f / height) / wobble_wavelength + wobble_phase)) : 0.0f;
        }
        old_phase = wobble_phase; old_amplitude = amplitude;
        old_length = wobble_wavelength; old_height = height;
        old_top = top; old_bottom = bottom;
    }
    if (ripple_count)
        prepare_ripple_grid();
    float *out = wobble_vertices;
    for (unsigned cell = 0; cell < count / 4; ++cell) {
        const float *tl = vertices + cell * 40;
        float influence[2];
        float impact[2][WOBBLE_ROWS + 1][2];
        for (unsigned side = 0; side < 2; ++side) {
            float x = tl[side * 10] * (1280.0f / width);
            /* PC wave coverage: 120-unit central quiet band, 580-unit edges. */
            float distance = fabsf(x - 640.0f) - 60.0f;
            influence[side] = distance > 0.0f ? distance / 580.0f * (width / 1280.0f) : 0.0f;
            if (ripple_count) {
                for (unsigned edge = 0; edge <= WOBBLE_ROWS; ++edge) {
                    float y = edge == WOBBLE_ROWS ? bottom :
                        top + (bottom - top) * ((float)edge / WOBBLE_ROWS);
                    ripple_displacement(x, y * (720.0f / height),
                        &impact[side][edge][0], &impact[side][edge][1]);
                }
            }
        }
        for (unsigned row = 0; row < WOBBLE_ROWS; ++row) {
            /* TL/TR/BR/BL, matching the native maze and GL_QUADS indices. */
            for (unsigned corner = 0; corner < 4; ++corner) {
                unsigned side = corner == 1 || corner == 2;
                unsigned edge = row + (corner >= 2);
                float t = (float)edge / WOBBLE_ROWS;
                const float *a = tl + side * 10, *b = tl + (3 - side) * 10;
                memcpy(out, a, 40);
                out[0] += influence[side] * row_wave[edge];
                /* Preserve exact endpoints and both current/old maze UVs. */
                for (unsigned i = 1; i <= 5; i += 2)
                    out[i] = edge == WOBBLE_ROWS ? b[i] : a[i] + (b[i] - a[i]) * t;
                if (ripple_count) {
                    out[0] += impact[side][edge][0] * (width / 1280.0f);
                    out[1] += impact[side][edge][1] * (height / 720.0f);
                }
                out += 10;
            }
        }
    }
}

int game_wobble_draw(const void *vertices, unsigned count, float light) {
    if ((!setting_mazeWobble && !setting_impactRipples) || !wobble_pacman || !vertices || !count ||
        count > WOBBLE_COLUMNS * 4 || count % 4)
        return 0;
    /* These flags are refreshed by the checked native Pac-Man update. Phase
     * advances only on its SetHorWave call, so paused rendering stays still. */
    if (!wobble_is_game() || wobble_is_preview() || !(wobble_pacman[30] & 1)) {
        wobble_amplitude = wobble_phase = 0.0f;
        ripple_count = 0;
        return 0;
    }
    if (!(wobble_pacman[33] & 1))
        wobble_amplitude = wobble_phase = 0.0f;
    if (!ripple_scene())
        ripple_count = 0;
    float width, height;
    if ((!ripple_count && (!setting_mazeWobble || wobble_amplitude < 1.0f)) ||
        !game_shader_screen_size(&width, &height) || width > 4096.0f || height > 4096.0f ||
        !rectangular_columns(vertices, count))
        return 0;
    game_wobble_prepare();
    if (!wobble_vertices)
        return 0;
    build_wobble_mesh(vertices, count, width, height);
    GLint array;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 40, wobble_vertices);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 40, wobble_vertices + 2);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 40, wobble_vertices + 4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 40, wobble_vertices + 6);
    for (unsigned i = 1; i <= 4; ++i) glEnableVertexAttribArray(i);
    glDisableVertexAttribArray(0);
    glVertexAttrib1f(0, light);
    /* Unlike the indexed path, this shares one bounded, GPU-owned upload
     * across all four arrays. The CPU mesh can be reused after the call. */
    glDrawArrays(GL_QUADS, 0, count * WOBBLE_ROWS);
    glBindBuffer(GL_ARRAY_BUFFER, array);
    /* The caller restores the native attribute pointers after this draw. */
    return 1;
}
