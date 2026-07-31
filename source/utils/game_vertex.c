#include "game_vertex.h"
#include "game_patch.h"
#include <string.h>

static void **graphics_instance;

static void transform_point(void *graphics, uint32_t *x, uint32_t *y) {
    void (*transform)(uint32_t *, uint32_t *);
    memcpy(&transform, (char *)graphics + 0x14, sizeof(transform));
    if (transform)
        transform(x, y);
}

static void vertex_set(void *vertex, uint32_t x, uint32_t y,
                       uint32_t u, uint32_t v, const void *color) {
    transform_point(*graphics_instance, &x, &y);
    memcpy((char *)vertex, &x, sizeof(x));
    memcpy((char *)vertex + 4, &y, sizeof(y));
    memcpy((char *)vertex + 8, &u, sizeof(u));
    memcpy((char *)vertex + 12, &v, sizeof(v));
    for (size_t i = 0; i < 4; ++i) {
        uint32_t component;
        memcpy(&component, (const char *)color + i * 4, sizeof(component));
        memcpy((char *)vertex + 16 + i * 4, &component, sizeof(component));
    }
}

void game_vertex_install_hooks(void) {
    uintptr_t vertex = game_patch_checked_function(
        "_ZN3sys19cVertexSpriteNormal9SetVertexEffffRKNS_7Color4fE",
        0xb8, 0xba101fffu);
    uintptr_t transform = game_patch_checked_function(
        "_ZN3sys8Graphics15TransformPointfERfS1_", 0x3e, 0xb90261bcu);
    graphics_instance = (void **)so_symbol(&so_mod, "_ZN3sys11g_pGraphicsE");
    if (vertex && transform && graphics_instance) {
        hook_addr(vertex, (uintptr_t)vertex_set);
        hook_addr(transform, (uintptr_t)transform_point);
    }
}
