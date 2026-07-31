#include "game_transform.h"
#include "game_patch.h"
#include <string.h>

#pragma GCC optimize ("no-fast-math")

static uint32_t (*wrap_angle)(uint32_t angle);
static void (*angle_sincos)(uint32_t angle, float *sine, float *cosine);

static float number(const void *object, size_t offset) {
    float value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

/* Android returns this nontrivial cVector2 through a hidden first pointer,
 * and passes the input vector by address. Keep the original rounding order
 * and native angle wrapping. Nonzero angles retain native trigonometry. */
static void *sprite_conv_pos(void *result, const void *sprite,
                             const void *point, uint32_t unscaled) {
    float cx = number(sprite, 0x44) * number(sprite, 0x5c);
    float cy = number(sprite, 0x48) * number(sprite, 0x60);
    uint32_t angle_bits;
    memcpy(&angle_bits, (const char *)sprite + 0x74, sizeof(angle_bits));
    angle_bits = wrap_angle(angle_bits);
    float angle;
    memcpy(&angle, &angle_bits, sizeof(angle));
    angle *= 0x1.921fb6p+2f;
    memcpy(&angle_bits, &angle, sizeof(angle_bits));

    /* ConvPos constructs the output before reading the input point. */
    memset(result, 0, 2 * sizeof(float));
    float x = number(point, 0) - cx;
    float y = number(point, 4) - cy;
    if (!(unscaled & 1)) {
        x *= number(sprite, 0x6c);
        y *= number(sprite, 0x70);
    }
    float sine, cosine;
    /* Exact zero-angle results; retain the rotation arithmetic below for
     * signed zero and exceptional coordinates. */
    if (!(angle_bits & 0x7fffffffu)) {
        sine = angle;
        cosine = 1.0f;
    } else {
        angle_sincos(angle_bits, &sine, &cosine);
    }
    float rx, ry;
    /* Match cMatrix44::Transform's non-fused VMLA operations exactly. C
     * contraction can otherwise turn (-sine)*y into -(sine*y), changing
     * exceptional results even with fast-math disabled. */
    __asm__(
        "vneg.f32 %0, %2\n"
        "vmul.f32 %0, %0, %5\n"
        "vmla.f32 %0, %3, %4\n"
        "vmul.f32 %1, %3, %5\n"
        "vmla.f32 %1, %2, %4\n"
        : "=&t" (rx), "=&t" (ry)
        : "t" (sine), "t" (cosine), "t" (x), "t" (y));
    memcpy(result, &rx, sizeof(rx));
    memcpy((char *)result + 4, &ry, sizeof(ry));
    rx = number(result, 0) + number(sprite, 0x54);
    memcpy(result, &rx, sizeof(rx));
    ry = number(result, 4) + number(sprite, 0x58);
    memcpy((char *)result + 4, &ry, sizeof(ry));
    return result;
}

void game_transform_install_hooks(void) {
    uintptr_t transform = game_patch_checked_function(
        "_ZN3sys7cSprite7ConvPosENS_8cVector2Eb", 0x18c, 0xd7692dceu);
    if (!transform)
        return;
    wrap_angle = (void *)game_patch_checked_function(
        "_ZN3sys5cMath4HalfEf", 0x6e, 0xbfb8c072u);
    angle_sincos = (void *)game_patch_checked_function(
        "_ZN3sys5cMath7sincosfEfPfS1_", 0x36, 0xe4e2be44u);
    if (wrap_angle && angle_sincos)
        hook_addr(transform, (uintptr_t)sprite_conv_pos);
}
