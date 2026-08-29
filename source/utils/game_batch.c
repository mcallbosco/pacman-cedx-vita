#include "game_batch.h"
#include "game_patch.h"
#include "glutil.h"
#include <arm_neon.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#pragma GCC optimize ("no-fast-math")

extern void *dlsym_soloader(void *handle, const char *symbol);

static void (*transform_integer)(void *, void *, void *);
static void (*transform_float)(void *, void *, void *);
static void (*draw_arrays)(GLenum, GLint, GLsizei);
static uintptr_t batch_resume __attribute__((used));
static uintptr_t batch_capacity_resume __attribute__((used));
static void (*flush_batches)(void *);
static GLuint sprite_indices;
static int indices_failed;
static int direct_grid_supported;
static int sprite_batch_supported;

/* The checked native Graphics constructor reserves 0xfb1e0 bytes. Keep room
 * for expansion to six 32-byte vertices if a batch later mixes geometry. */
enum { SPRITE_BATCH_QUADS = 0xfb1e0 / (6 * 32) };

int game_batch_direct_grid_supported(void) {
    return direct_grid_supported;
}
static struct {
    void *graphics, *buffer;
    unsigned vertices, stride;
} direct_grid;

/* Keep the native six-vertex count for capacity checks. Only the physical
 * cursor is compact; a mixed batch expands before any ordinary writer runs. */
static struct {
    void *graphics;
    uint32_t *buffer;
    unsigned quads;
} direct_sprites;

void game_batch_direct_grid(void *graphics, void *buffer, unsigned vertices, unsigned stride) {
    direct_grid.graphics = graphics;
    direct_grid.buffer = buffer;
    direct_grid.vertices = vertices;
    direct_grid.stride = stride;
}

/* Ordinary sprites use 0,1,2,0,4,2; grids use 0,1,2,1,4,2. The latter can use
 * vitaGL's existing quad index buffer. Allocate the former once, on the render
 * thread, retaining the original draw if allocation fails. */
static int prepare_sprite_indices(void) {
    if (sprite_indices)
        return 1;
    if (indices_failed)
        return 0;
    indices_failed = 1;
    const size_t bytes = 0xc000 * sizeof(uint16_t);
    uint16_t *indices = malloc(bytes);
    if (!indices)
        return 0;
    for (uint32_t i = 0; i < 0xc000 / 6; ++i) {
        uint32_t vertex = i * 4;
        uint16_t *dst = indices + i * 6;
        dst[0] = vertex;
        dst[1] = vertex + 1;
        dst[2] = vertex + 3;
        dst[3] = vertex;
        dst[4] = vertex + 2;
        dst[5] = vertex + 3;
    }
    GLuint buffer = 0;
    GLint previous, size = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previous);
    glGenBuffers(1, &buffer);
    if (buffer) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, bytes, indices, GL_STATIC_DRAW);
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, previous);
        if (size == bytes)
            sprite_indices = buffer;
        else
            glDeleteBuffers(1, &buffer);
    }
    free(indices);
    return sprite_indices != 0;
}

static inline void copy_vertex(uint32_t *dst, const uint32_t *src, size_t words) {
    vst1q_u32(dst, vld1q_u32(src));
    vst1q_u32(dst + 4, vld1q_u32(src + 4));
    if (words == 10)
        vst1_u32(dst + 8, vld1_u32(src + 8));
}

static void expand_sprites(void *graphics) {
    if (direct_sprites.graphics != graphics)
        return;
    uint32_t *buffer = direct_sprites.buffer;
    unsigned quads = direct_sprites.quads;
    direct_sprites.graphics = NULL;
    /* Expand backwards; a local quad protects overlapping source vertices. */
    for (unsigned q = quads; q--;) {
        uint32_t vertices[4][8];
        memcpy(vertices, buffer + q * 32, sizeof(vertices));
        static const unsigned order[] = {0, 1, 3, 0, 2, 3};
        for (unsigned i = 0; i < 6; ++i)
            copy_vertex(buffer + (q * 6 + i) * 8, vertices[order[i]], 8);
    }
    void *cursor = buffer + quads * 48;
    memcpy((char *)graphics + 0x3c, &cursor, sizeof(cursor));
}

int game_batch_append_sprite(void *graphics, const uint32_t vertices[4][8]) {
    if (!sprite_batch_supported)
        return 0;
    uint32_t count;
    uint32_t *buffer, *cursor;
    memcpy(&count, (char *)graphics + 0x40, 4);
    memcpy(&buffer, (char *)graphics + 0x38, 4);
    memcpy(&cursor, (char *)graphics + 0x3c, 4);
    if (!buffer || !cursor || ((uintptr_t)cursor & 3))
        return 0;
    if (count > SPRITE_BATCH_QUADS * 6 - 6) {
        uintptr_t vtable, flush;
        memcpy(&vtable, graphics, 4);
        memcpy(&flush, (const char *)vtable + 60, 4);
        if (!flush_batches || flush != (uintptr_t)flush_batches)
            return 0;
        /* Native BeginBatches compares its vertex count against the byte
         * capacity. Flush a full sprite batch while its expansion still fits. */
        flush_batches(graphics);
        memcpy(&count, (char *)graphics + 0x40, 4);
        memcpy(&buffer, (char *)graphics + 0x38, 4);
        memcpy(&cursor, (char *)graphics + 0x3c, 4);
    }
    if (!count && direct_sprites.graphics == graphics)
        direct_sprites.graphics = NULL;
    int compact = count < SPRITE_BATCH_QUADS * 6 && gl_batch_can_index(32) &&
        ((!count && cursor == buffer && !direct_sprites.graphics && prepare_sprite_indices()) ||
         (direct_sprites.graphics == graphics && direct_sprites.buffer == buffer &&
          count == direct_sprites.quads * 6 && cursor == buffer + direct_sprites.quads * 32));
    if (compact) {
        if (!count) {
            direct_sprites.graphics = graphics;
            direct_sprites.buffer = buffer;
            direct_sprites.quads = 0;
        }
        memcpy(cursor, vertices, 4 * 32);
        cursor += 32;
        ++direct_sprites.quads;
    } else {
        expand_sprites(graphics);
        memcpy(&cursor, (char *)graphics + 0x3c, 4);
        static const unsigned order[] = {0, 1, 3, 0, 2, 3};
        for (unsigned i = 0; i < 6; ++i)
            copy_vertex(cursor + i * 8, vertices[order[i]], 8);
        cursor += 48;
    }
    count += 6;
    *((uint8_t *)graphics + 0x85) = 0;
    memcpy((char *)graphics + 0x3c, &cursor, 4);
    memcpy((char *)graphics + 0x40, &count, 4);
    return 1;
}

static void append_vertex(void *graphics, const uint32_t *values, size_t size) {
    /* A custom transform can flush and recursively emit another sprite. */
    expand_sprites(graphics);
    void *cursor;
    uint32_t count;
    /* Read these after the transform callback, which may change graphics state. */
    memcpy(&cursor, (char *)graphics + 0x3c, sizeof(cursor));
    copy_vertex(cursor, values, size / sizeof(*values));
    cursor = (char *)cursor + size;
    memcpy((char *)graphics + 0x3c, &cursor, sizeof(cursor));
    memcpy(&count, (char *)graphics + 0x40, sizeof(count));
    ++count;
    memcpy((char *)graphics + 0x40, &count, sizeof(count));
}

static void integer_position(void *graphics, uint32_t *values, uint8_t subtexture) {
    expand_sprites(graphics);
    *((uint8_t *)graphics + 0x85) = subtexture;
    transform_integer(graphics, &values[0], &values[1]);
    for (size_t i = 0; i < 2; ++i) {
        int32_t coordinate;
        memcpy(&coordinate, &values[i], sizeof(coordinate));
        float value = (float)coordinate;
        memcpy(&values[i], &value, sizeof(value));
    }
}

static void float_position(void *graphics, uint32_t *values, uint8_t subtexture) {
    expand_sprites(graphics);
    *((uint8_t *)graphics + 0x85) = subtexture;
    transform_float(graphics, &values[0], &values[1]);
}

/* Keep the Android softfp arguments as bits, including UVs, colours and NaNs.
 * Each original transform still runs, in order, before packing the vertex. */
static void add_batch(void *graphics, uint32_t x, uint32_t y, uint32_t u,
                      uint32_t v, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    uint32_t values[] = {x, y, u, v, r, g, b, a};
    integer_position(graphics, values, 0);
    append_vertex(graphics, values, sizeof(values));
}

static void add_batch_f(void *graphics, uint32_t x, uint32_t y, uint32_t u,
                        uint32_t v, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    uint32_t values[] = {x, y, u, v, r, g, b, a};
    float_position(graphics, values, 0);
    append_vertex(graphics, values, sizeof(values));
}

static void add_batch_sub(void *graphics, uint32_t x, uint32_t y, uint32_t u,
                          uint32_t v, uint32_t su, uint32_t sv, uint32_t r,
                          uint32_t g, uint32_t b, uint32_t a) {
    uint32_t values[] = {x, y, u, v, su, sv, r, g, b, a};
    integer_position(graphics, values, 1);
    append_vertex(graphics, values, sizeof(values));
}

static void add_batch_sub_f(void *graphics, uint32_t x, uint32_t y, uint32_t u,
                            uint32_t v, uint32_t su, uint32_t sv, uint32_t r,
                            uint32_t g, uint32_t b, uint32_t a) {
    uint32_t values[] = {x, y, u, v, su, sv, r, g, b, a};
    float_position(graphics, values, 1);
    append_vertex(graphics, values, sizeof(values));
}

static inline int same_vertex(const uint32_t *a, const uint32_t *b, size_t words) {
    uint32x4_t different = vorrq_u32(veorq_u32(vld1q_u32(a), vld1q_u32(b)),
                                    veorq_u32(vld1q_u32(a + 4), vld1q_u32(b + 4)));
    uint32x2_t halves = vorr_u32(vget_low_u32(different), vget_high_u32(different));
    uint32_t result = vget_lane_u32(halves, 0) | vget_lane_u32(halves, 1);
    if (words == 10)
        result |= (a[8] ^ b[8]) | (a[9] ^ b[9]);
    return result == 0;
}

/* Validate the whole batch before changing its disposable CPU buffer. Some
 * grids intentionally give shared corners different colours or UVs; those
 * must remain independent vertices. Compare bits, not floating-point values.
 * Both index patterns use the four stored vertices old 0,1,4,2. */
#define DEFINE_COMPACT(words) \
static int compact_##words(void *buffer, uint32_t quads) { \
    typedef struct { uint32_t value[words]; } Vertex; \
    Vertex *vertices = buffer; \
    uint32_t shared = same_vertex(vertices[1].value, vertices[3].value, words) ? 1 : 0; \
    for (uint32_t i = 0; i < quads; ++i) { \
        const Vertex *src = vertices + i * 6; \
        if (!same_vertex(src[shared].value, src[3].value, words) || \
            !same_vertex(src[2].value, src[5].value, words)) \
            return 0; \
    } \
    if (!shared && !prepare_sprite_indices()) return 0; \
    for (uint32_t i = 0; i < quads; ++i) { \
        const Vertex *src = vertices + i * 6; \
        Vertex *dst = vertices + i * 4; \
        uint32x4_t third_lo = vld1q_u32(src[2].value); \
        uint32x4_t third_hi = vld1q_u32(src[2].value + 4); \
        uint32x2_t third_tail; \
        if (words == 10) third_tail = vld1_u32(src[2].value + 8); \
        copy_vertex(dst[0].value, src[0].value, words); \
        copy_vertex(dst[1].value, src[1].value, words); \
        copy_vertex(dst[2].value, src[4].value, words); \
        vst1q_u32(dst[3].value, third_lo); \
        vst1q_u32(dst[3].value + 4, third_hi); \
        if (words == 10) vst1_u32(dst[3].value + 8, third_tail); \
    } \
    return shared ? 1 : 2; \
}

DEFINE_COMPACT(8)
DEFINE_COMPACT(10)

static void __attribute__((used, noinline)) draw_batch(
        GLenum mode, GLint first, GLsizei count, void *graphics) {
    if (direct_sprites.graphics == graphics) {
        void *buffer, *cursor;
        memcpy(&buffer, (char *)graphics + 0x38, 4);
        memcpy(&cursor, (char *)graphics + 0x3c, 4);
        unsigned quads = direct_sprites.quads;
        if (mode == GL_TRIANGLES && first == 0 && count == quads * 6 &&
            buffer == direct_sprites.buffer && cursor == direct_sprites.buffer + quads * 32 &&
            gl_batch_can_index(32)) {
            direct_sprites.graphics = NULL;
            gl_draw_indexed_batch(sprite_indices, quads * 4);
            return;
        }
        expand_sprites(graphics);
    }
    if (direct_grid.graphics == graphics) {
        void *buffer;
        memcpy(&buffer, (char *)graphics + 0x38, sizeof(buffer));
        int direct = mode == GL_TRIANGLES && first == 0 &&
            count == direct_grid.vertices && buffer == direct_grid.buffer;
        direct_grid.graphics = NULL;
        if (direct) {
            gl_draw_direct_grid(buffer, count, direct_grid.stride);
            return;
        }
    }
    /* The bundled vitaGL allocates 0xc000 quad indices (8192 quads). Larger
     * or non-quad submissions retain their original draw path and buffer. */
    if (mode == GL_TRIANGLES && first == 0 && count >= 6 &&
        count <= 0xc000 && count % 6 == 0) {
        uintptr_t buffer, cursor;
        uint32_t vertices;
        memcpy(&buffer, (char *)graphics + 0x38, sizeof(buffer));
        memcpy(&cursor, (char *)graphics + 0x3c, sizeof(cursor));
        memcpy(&vertices, (char *)graphics + 0x40, sizeof(vertices));
        uint32_t stride = (*((uint8_t *)graphics + 0x85) & 1) ? 40 : 32;
        if (buffer && !(buffer & 3) && vertices == (uint32_t)count &&
            cursor >= buffer && cursor - buffer == vertices * stride &&
            gl_batch_can_index(stride)) {
            uint32_t quads = vertices / 6;
            int compacted = stride == 32 ? compact_8((void *)buffer, quads)
                                         : compact_10((void *)buffer, quads);
            if (compacted == 2) {
                gl_draw_indexed_batch(sprite_indices, quads * 4);
                return;
            }
            if (compacted == 1) {
                mode = GL_QUADS;
                count = quads * 4;
            }
        }
    }
    /* Use the same import wrapper as the original call, including the map
     * shader's attribute remapping and optional debug instrumentation. vitaGL
     * copies client vertices into its existing GPU pool before returning. */
    draw_arrays(mode, first, count);
}

/* Replace only Flush's draw call and its two preceding instructions. Preserve
 * native setup/cleanup, including resetting the count after submission. */
static void __attribute__((naked)) batch_bridge(void) {
    __asm__ volatile(
        "movs r1, #0\n"
        "str r1, [sp, #12]\n"
        "ldr r3, [sp, #40]\n"
        "push {r4, lr}\n"
        "bl draw_batch\n"
        "pop {r4, lr}\n"
        "ldr ip, =batch_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n"
        ".ltorg\n");
}

static unsigned __attribute__((used, noinline)) batch_capacity(unsigned stride) {
    if (stride == 32 || stride == 40)
        return (0xfb1e0 - 1) / stride;
    return 0xfb1df;
}

static void __attribute__((naked)) batch_capacity_bridge(void) {
    __asm__ volatile(
        "push {r0, r1, r3, r5, r12, lr}\n"
        "mov r0, r4\n"
        "bl batch_capacity\n"
        "mov r2, r0\n"
        "pop {r0, r1, r3, r5, r12, lr}\n"
        "ldr r12, =batch_capacity_resume\n"
        "ldr r12, [r12]\n"
        "bx r12\n"
        ".ltorg\n");
}

void game_batch_install_hooks(void) {
    uintptr_t flush = game_patch_checked_function(
        "_ZN3sys22GraphicsAndroidShaders5FlushEv", 0x220, 0x686cadc6u);
    uintptr_t integer = game_patch_checked_function(
        "_ZN3sys8Graphics8AddBatchEiiffffff", 0x120, 0xcdb0d23du);
    uintptr_t floating = game_patch_checked_function(
        "_ZN3sys8Graphics9AddBatchfEffffffff", 0x118, 0x172468b0u);
    uintptr_t integer_sub = game_patch_checked_function(
        "_ZN3sys8Graphics8AddBatchEiiffffffff", 0x150, 0xea5f70ceu);
    uintptr_t floating_sub = game_patch_checked_function(
        "_ZN3sys8Graphics9AddBatchfEffffffffff", 0x148, 0x802accf8u);
    transform_integer = (void *)game_patch_checked_function(
        "_ZN3sys8Graphics15TransformPointiERiS1_", 0x84, 0xc3cf8d4cu);
    transform_float = (void *)game_patch_checked_function(
        "_ZN3sys8Graphics15TransformPointfERfS1_", 0x3e, 0xb90261bcu);
    uintptr_t subtexture = game_patch_checked_function(
        "_ZN3sys8Graphics16SetUseSubTextureEb", 0x22, 0x918d03c9u);
    if (!flush || !integer || !floating || !integer_sub || !floating_sub ||
        !transform_integer || !transform_float || !subtexture)
        return;
    draw_arrays = dlsym_soloader(NULL, "glDrawArrays");
    if (!draw_arrays)
        return;

    batch_resume = flush + 0x1cc;
    flush_batches = (void *)flush;
    hook_addr(flush + 0x1c4, (uintptr_t)batch_bridge);
    hook_addr(integer, (uintptr_t)add_batch);
    hook_addr(floating, (uintptr_t)add_batch_f);
    hook_addr(integer_sub, (uintptr_t)add_batch_sub);
    hook_addr(floating_sub, (uintptr_t)add_batch_sub_f);
    direct_grid_supported = 1;
    uintptr_t begin = game_patch_checked_function(
        "_ZN3sys8Graphics12BeginBatchesENS_13PrimitiveTypeEjjNS_9BlendTypeEh",
        0xfc, 0x3a5847e5u);
    if (begin && game_patch_checked_function(
            "_ZN3sys8GraphicsC2Ev", 0xb0, 0x6683dce6u)) {
        /* Native code compares vertices with its byte allocation. Correct
         * that bound before its existing flush decision, including mixed
         * batches that may need the compact sprites expanded in place. */
        batch_capacity_resume = begin + 0x46;
        hook_addr(begin + 0x3e, (uintptr_t)batch_capacity_bridge);
        sprite_batch_supported = 1;
    }
}
