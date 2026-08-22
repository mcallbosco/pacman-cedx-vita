#include "game_texture.h"
#include "game_patch.h"

#include <arm_neon.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

static uintptr_t texture_resume __attribute__((used));
static uintptr_t texture_done __attribute__((used));
static const char *(*texture_filename)(void *texture);

static uint32_t word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

/* Square RGBA8 atlases use Morton order, with X in odd and Y in even bits.
 * Copy complete 4x4 tiles using the same ordering as vitaGL's swizzler. */
static void swizzle_rgba(uint32_t *destination, const uint32_t *source,
                         uint32_t size) {
    uint32_t x_mask = 0xaaaaaaa0u | ~(size * size - 1);
    uint32_t y_mask = 0x55555550u | ~(size * size - 1);
    uint32_t y_bits = 0;
    for (uint32_t y = 0; y < size; y += 4) {
        const uint32_t *row = source + y * size;
        uint32_t x_bits = 0;
        for (uint32_t x = 0; x < size; x += 4, row += 4) {
            uint32x4x2_t upper = vzipq_u32(vld1q_u32(row),
                                          vld1q_u32(row + size));
            uint32x4x2_t lower = vzipq_u32(vld1q_u32(row + size * 2),
                                          vld1q_u32(row + size * 3));
            uint32_t *tile = destination + (x_bits | y_bits);
            vst1q_u32(tile, upper.val[0]);
            vst1q_u32(tile + 4, lower.val[0]);
            vst1q_u32(tile + 8, upper.val[1]);
            vst1q_u32(tile + 12, lower.val[1]);
            x_bits = (x_bits - x_mask) & x_mask;
        }
        y_bits = (y_bits - y_mask) & y_mask;
    }
}

static int maze_atlas(const char *path) {
    if (!path)
        return 0;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    size_t length = strlen(name);
    return length > 15 && !strncmp(name, "pac_ce_maze", 11) &&
           !strcmp(name + length - 4, ".png");
}

static void __attribute__((used, noinline)) swizzle_uploaded_texture(void *texture) {
    /* GenerateTexture uploads a new name and one mip level. Its separate
     * CreateTexture render-target allocator never enters this hook. */
    uint32_t size = word(texture, 36);
    const uint32_t *source = (const void *)(uintptr_t)word(texture, 44);
    if (word(texture, 48) != 0 || !source || ((uintptr_t)source & 3) ||
        size < 64 || size > 1024 || (size & (size - 1)) ||
        word(texture, 40) != size || !word(texture, 32) ||
        !maze_atlas(texture_filename(texture)))
        return;

    GLint bound;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if ((uint32_t)bound != word(texture, 32))
        return;
    SceGxmTexture *descriptor = vglGetGxmTexture(GL_TEXTURE_2D);
    void *pixels = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (!descriptor || !pixels)
        return;
    /* Bundled vitaGL keeps these flags immediately before gxm_tex, independent
     * of optional cache fields (shared.h's texture). Failed uploads can leave
     * stale descriptors in reused slots; never read their old allocation. */
    const uint8_t *flags = (const uint8_t *)descriptor - 8;
    if (flags[0] != 2 || flags[1] != 1 || flags[4] ||
        pixels == source || ((uintptr_t)pixels & 3) ||
        sceGxmTextureGetType(descriptor) != SCE_GXM_TEXTURE_LINEAR ||
        sceGxmTextureGetFormat(descriptor) != SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR ||
        sceGxmTextureGetWidth(descriptor) != size ||
        sceGxmTextureGetHeight(descriptor) != size ||
        sceGxmTextureGetData(descriptor) != pixels ||
        sceGxmTextureGetMinFilter(descriptor) != SCE_GXM_TEXTURE_FILTER_LINEAR ||
        sceGxmTextureGetMagFilter(descriptor) != SCE_GXM_TEXTURE_FILTER_LINEAR ||
        sceGxmTextureGetMipFilter(descriptor) != SCE_GXM_TEXTURE_MIP_FILTER_DISABLED)
        return;

    SceGxmTexture swizzled;
    if (sceGxmTextureInitSwizzledArbitrary(&swizzled, pixels,
            SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, size, size, 1) < 0)
        return;

    /* Linear and swizzled-arbitrary descriptors encode dimensions identically.
     * Take only the SDK-generated layout bits, preserving every sampler bit
     * and the existing vitaGL-owned allocation, including its mip settings. */
    if (swizzled.generic2.width != descriptor->generic2.width ||
        swizzled.generic2.height != descriptor->generic2.height ||
        swizzled.generic2.base_format != descriptor->generic2.base_format ||
        sceGxmTextureGetType(&swizzled) != SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY)
        return;
    SceGxmTexture replacement = *descriptor;
    replacement.generic2.type = swizzled.generic2.type;

    size_t bytes = size * size * sizeof(*source);
    uint32_t *temporary = malloc(bytes);
    if (!temporary)
        return;
    swizzle_rgba(temporary, source, size);
    memcpy(pixels, temporary, bytes);
    *descriptor = replacement;
    free(temporary);
}

/* The verified uploader has completed glTexImage2D and still owns its PNG
 * bytes. Preserve the original retain/free decision and its condition flags. */
static void __attribute__((naked)) texture_bridge(void) {
    __asm__ volatile(
        "push {r0, r1, r2, r3, r4, lr}\n"
        "ldr r0, [sp, #112]\n"
        "bl swizzle_uploaded_texture\n"
        "pop {r0, r1, r2, r3, r4, lr}\n"
        "cmp r0, #0\n"
        "bne 1f\n"
        "ldr r0, [sp, #88]\n"
        "ldr ip, =texture_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n"
        "1: ldr ip, =texture_done\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

void game_texture_install_hooks(void) {
    uintptr_t upload = game_patch_checked_function(
        "_ZN3sys22GraphicsAndroidShaders15GenerateTextureEPNS_7TextureEb",
        0x20c, 0xc4e7f651u);
    uintptr_t filename = game_patch_checked_function(
        "_ZN3sys7Texture19GetCompleteFilenameEv", 0x10, 0xeb454898u);
    if (!upload || !filename)
        return;
    texture_filename = (void *)filename;
    texture_resume = upload + 0x1f0;
    texture_done = upload + 0x202;
    hook_addr(upload + 0x1e8, (uintptr_t)texture_bridge);
}
