#include "game_wave.h"
#include "game_patch.h"

#include <math.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math", "fp-contract=off")

static float (*vector_length)(const void *vector);
static void *(*vector_normalize)(const void *vector, void *result);

static uint32_t word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static float number(const void *object, size_t offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static void set_number(void *object, size_t offset, float value) {
    memcpy((char *)object + offset, &value, sizeof(value));
}

/* C may commute multiplication even without fast-math. Native operand order
 * determines the result payload when both operands are NaNs. */
static float multiply(float direction, float position) {
    float result;
    __asm__("vmul.f32 %0, %1, %2"
            : "=t" (result) : "t" (direction), "t" (position));
    return result;
}

/* Android grid points have a 52-byte stride, with displacement at +28/+32.
 * Their 20-byte wave records hold position, velocity, direction and two flag
 * bytes.*/
static void wave_calc(void *task) {
    const uint8_t *enabled = (const void *)(uintptr_t)word(task, 36);
    if (!(*enabled & 1))
        return;

    const void *sprite = (const void *)(uintptr_t)word(task, 32);
    int32_t count = (int32_t)((word(sprite, 32) + 1) * (word(sprite, 36) + 1));
    uint8_t *point = (void *)(uintptr_t)word(sprite, 28);
    uint8_t *wave = (void *)(uintptr_t)word(task, 28);
    const float spring = number(task, 40);
    const float damping = number(task, 44);
    for (int32_t i = 0; i < count; ++i, point += 52, wave += 20) {
        if (wave[17] & 1) {
            float position = vector_length(point + 28);
            set_number(wave, 0, position);
            if (position > 0.1f) {
                set_number(wave, 4, 0.0f);
                vector_normalize(point + 28, wave + 8);
                wave[16] = 1;
            }
            wave[17] = 0;
        } else if (wave[16] & 1) {
            float position = number(wave, 0);
            float velocity = number(wave, 4);
            if (fabsf(position) > 0.5f || fabsf(velocity) > damping) {
                /* VMLS rounds the product before subtraction. Retain that
                 * instruction and operand order, including exceptional floats. */
                __asm__("vmls.f32 %0, %1, %2"
                        : "+t" (velocity) : "t" (position), "t" (spring));
                if (position > 0.0f) {
                    if (velocity > 0.0f)
                        velocity -= damping;
                } else if (velocity < 0.0f) {
                    velocity += damping;
                }
                position += velocity;
                set_number(wave, 4, velocity);
                set_number(wave, 0, position);
                set_number(point, 28, multiply(number(wave, 8), position));
                set_number(point, 32, multiply(number(wave, 12), position));
            } else {
                set_number(point, 28, 0.0f);
                set_number(point, 32, 0.0f);
                wave[16] = 0;
            }
        }
    }
}

void game_wave_install_hooks(void) {
    uintptr_t wave = game_patch_checked_function(
        "_ZN9newPacman21cTsTaskEffectWaveCalc4FuncEv", 0x278, 0x02053483u);
    if (!wave ||
        !game_patch_checked_function("_ZN3sys7cSprite12GetGridPointEjj", 0x38, 0xb6c115cfu) ||
        !game_patch_checked_function("_ZN3sys7cSprite17GetGridPointWidthEv", 0x12, 0xc34fd6d3u) ||
        !game_patch_checked_function("_ZN3sys7cSprite18GetGridPointHeightEv", 0x12, 0xe049ce13u))
        return;

    vector_length = (void *)game_patch_checked_function(
        "_ZN3sys8cVector26LengthEv", 0x26, 0x6d4a4b31u);
    vector_normalize = (void *)game_patch_checked_function(
        "_ZN3sys8cVector29NormalizeEPS0_", 0x56, 0xbfa5ce5fu);
    if (vector_length && vector_normalize)
        hook_addr(wave, (uintptr_t)wave_calc);
}
