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
       WOBBLE_VERTICES = WOBBLE_COLUMNS * WOBBLE_ROWS * 4 };

static so_hook horizontal_hook;
static const unsigned char *wobble_pacman;
static int (*wobble_is_game)(void);
static int (*wobble_is_preview)(void);
static float wobble_amplitude, wobble_phase, wobble_wavelength;
static float *wobble_vertices;
static int wobble_attempted;

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
    if (!setting_mazeWobble || setting_lowPerformance)
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
    if (wobble_pacman)
        horizontal_hook = hook_addr(wave, (uintptr_t)horizontal_wave);
}

void game_wobble_prepare(void) {
    if (!setting_mazeWobble || !wobble_pacman || wobble_attempted)
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

static void build_wobble_mesh(const float *vertices, unsigned count,
                             float width, float height) {
    /* Cache the shared row displacement across all layers of the same frame.
     * Only 37 sines are evaluated, rather than one for every maze vertex. */
    static float row_wave[WOBBLE_ROWS + 1];
    static float old_phase = -1.0f, old_amplitude, old_length, old_height;
    static float old_top, old_bottom;
    float top = vertices[1], bottom = vertices[31];
    if (old_phase != wobble_phase || old_amplitude != wobble_amplitude ||
        old_length != wobble_wavelength || old_height != height ||
        old_top != top || old_bottom != bottom) {
        for (unsigned row = 0; row <= WOBBLE_ROWS; ++row) {
            float y = top + (bottom - top) * ((float)row / WOBBLE_ROWS);
            row_wave[row] = (0.35f * wobble_amplitude) * wobble_sine(6.28318530718f *
                (y * (720.0f / height) / wobble_wavelength + wobble_phase));
        }
        old_phase = wobble_phase; old_amplitude = wobble_amplitude;
        old_length = wobble_wavelength; old_height = height;
        old_top = top; old_bottom = bottom;
    }
    float *out = wobble_vertices;
    for (unsigned cell = 0; cell < count / 4; ++cell) {
        const float *tl = vertices + cell * 40;
        float influence[2];
        for (unsigned side = 0; side < 2; ++side) {
            float x = tl[side * 10] * (1280.0f / width);
            /* PC wave coverage: 120-unit central quiet band, 580-unit edges. */
            float distance = fabsf(x - 640.0f) - 60.0f;
            influence[side] = distance > 0.0f ? distance / 580.0f * (width / 1280.0f) : 0.0f;
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
                out += 10;
            }
        }
    }
}

int game_wobble_draw(const void *vertices, unsigned count, float light) {
    if (!setting_mazeWobble || !wobble_pacman || !vertices || !count ||
        count > WOBBLE_COLUMNS * 4 || count % 4)
        return 0;
    /* These flags are refreshed by the checked native Pac-Man update. Phase
     * advances only on its SetHorWave call, so paused rendering stays still. */
    if (!wobble_is_game() || wobble_is_preview() ||
        !(wobble_pacman[30] & 1) || !(wobble_pacman[33] & 1)) {
        wobble_amplitude = wobble_phase = 0.0f;
        return 0;
    }
    float width, height;
    if (wobble_amplitude < 1.0f || !game_shader_screen_size(&width, &height) ||
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
