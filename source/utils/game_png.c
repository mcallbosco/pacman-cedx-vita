#include "game_png.h"
#include "game_patch.h"
#include <arm_neon.h>
#include <string.h>

static int16x4_t load_rgba(const unsigned char *pixel) {
    uint32_t packed;
    memcpy(&packed, pixel, sizeof(packed));
    return vreinterpret_s16_u16(vget_low_u16(vmovl_u8(vcreate_u8(packed))));
}

/* Each RGBA channel depends only on the same channel in the previous pixel.
 * Process the four channels together; keep intermediate differences signed
 * and wrap the reconstructed bytes before using them as the next predictor. */
static void png_paeth_rgba(unsigned char *row, const unsigned char *previous,
                           unsigned length) {
    int16x4_t left = vdup_n_s16(0), corner = vdup_n_s16(0);
    for (unsigned i = 0; i < length; i += 4) {
        int16x4_t above = load_rgba(previous + i);
        int16x4_t pa = vabs_s16(vsub_s16(above, corner));
        int16x4_t pb = vabs_s16(vsub_s16(left, corner));
        int16x4_t pc = vabs_s16(vsub_s16(vadd_s16(left, above),
                                        vshl_n_s16(corner, 1)));
        uint16x4_t choose_left = vand_u16(vcle_s16(pa, pb), vcle_s16(pa, pc));
        int16x4_t predictor = vbsl_s16(choose_left, left,
            vbsl_s16(vcle_s16(pb, pc), above, corner));
        left = vand_s16(vadd_s16(load_rgba(row + i), predictor), vdup_n_s16(255));
        int8x8_t bytes = vmovn_s16(vcombine_s16(left, left));
        uint32_t packed = vget_lane_u32(vreinterpret_u32_s8(bytes), 0);
        memcpy(row + i, &packed, sizeof(packed));
        corner = above;
    }
}

/* libpng's Android row-info layout stores rowbytes at +4 and pixel_depth at
 * +11. Reconstruct the original bytes without changing PNG decoding, texture
 * formats, or loader scheduling. All state belongs to the current row, so
 * background loads can use this hook without a shared cache or lock. */
static void png_filter_row(void *png, const void *info, unsigned char *row,
                           const unsigned char *previous, int filter) {
    (void)png;
    uint32_t length;
    memcpy(&length, (const char *)info + 4, sizeof(length));
    unsigned bpp = (((const unsigned char *)info)[11] + 7u) / 8u;
    unsigned leading = bpp < length ? bpp : length;

    switch (filter) {
    case 0:
        return;
    case 1:
        for (unsigned i = bpp; i < length; ++i)
            row[i] += row[i - bpp];
        return;
    case 2:
        for (unsigned i = 0; i < length; ++i)
            row[i] += previous[i];
        return;
    case 3:
        for (unsigned i = 0; i < leading; ++i)
            row[i] += previous[i] / 2u;
        for (unsigned i = bpp; i < length; ++i)
            row[i] += ((unsigned)row[i - bpp] + previous[i]) / 2u;
        return;
    case 4:
        if (bpp == 4 && length % 4 == 0) {
            png_paeth_rgba(row, previous, length);
            return;
        }
        for (unsigned i = 0; i < leading; ++i)
            row[i] += previous[i];
        for (unsigned i = bpp; i < length; ++i) {
            int left = row[i - bpp], above = previous[i];
            int corner = previous[i - bpp];
            int pa = above - corner, pb = left - corner, pc = pa + pb;
            if (pa < 0) pa = -pa;
            if (pb < 0) pb = -pb;
            if (pc < 0) pc = -pc;
            /* Preserve Paeth's left, above, corner tie-breaking order. */
            row[i] += pa <= pb && pa <= pc ? left : pb <= pc ? above : corner;
        }
        return;
    default:
        /* Preserve this library version's invalid-filter behavior. */
        row[0] = 0;
    }
}

void game_png_install_hooks(void) {
    uintptr_t png_filter = game_patch_checked_function(
        "png_read_filter_row", 0x2c4, 0xac843af9);
    if (png_filter)
        hook_addr(png_filter, (uintptr_t)png_filter_row);
}
