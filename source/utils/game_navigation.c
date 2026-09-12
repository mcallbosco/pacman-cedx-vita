#include "game_navigation.h"
#include "game_patch.h"
#include <limits.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math")

#define MAP_WIDTH 65
#define MAP_HEIGHT 47
#define MAP_CELLS (MAP_WIDTH * MAP_HEIGHT)

static int32_t (*navigation_map_x)(const void *, float);
static int32_t (*navigation_map_y)(const void *, float);
static uint32_t (*navigation_wall_elem)(const void *, float, float);
static void (*navigation_vector_allocate)(void *, unsigned);
static so_hook navigation_distance_hook, navigation_turn_hook;

static const int8_t step_x[4] = {1, 0, -1, 0};
static const int8_t step_y[4] = {0, -1, 0, 1};
static const uint8_t wall_attributes[26] = {
    0x41, 0x71, 0x61, 0, 0x20, 0, 0, 0x45, 0x49, 0,
    0x61, 0x61, 0x61, 0x61, 0x61, 0x41, 0x41, 0x41,
    0x41, 0, 0, 0, 0, 0x61, 0x61, 0x61
};

static uint32_t word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

static uint32_t navigation_wall_attribute(const void *map, uint8_t tile,
                                          int type, int direction) {
    if (tile >= sizeof(wall_attributes))
        return 0;
    /* Directional gates read the live map flag. Type 100, used by the path
     * search, passes all four gates regardless of their direction. */
    if (tile >= 15 && tile <= 18 && type < 100 && word(map, 0x8f60)) {
        static const int8_t gate_direction[4] = {1, 3, 2, 0};
        if (direction != gate_direction[tile - 15])
            return 1;
    }
    return wall_attributes[tile];
}

static uint32_t navigation_wall(const void *map, float x, float y,
                                int type, int direction) {
    return navigation_wall_attribute(map, navigation_wall_elem(map, x, y),
                                     type, direction);
}

static int wrap_x(int x) {
    return x <= 2 ? 61 : x >= 62 ? 3 : x;
}

static int wrap_y(int y) {
    return y <= 7 ? 38 : y >= 39 ? 8 : y;
}

static int navigation_distance(void *map, int sx, int sy, int tx, int ty,
                                int limit, int first_direction) {
    /* Unexpected coordinates retain the native assertions and conversions.
     * Normal callers supply GetMapX/Y's clamped coordinates. */
    if ((unsigned)sx >= MAP_WIDTH || (unsigned)tx >= MAP_WIDTH ||
        (unsigned)sy >= MAP_HEIGHT || (unsigned)ty >= MAP_HEIGHT)
        return SO_CONTINUE(int, navigation_distance_hook,
                           map, sx, sy, tx, ty, limit, first_direction);
    if (sx == tx && sy == ty)
        return 0;
    int dx = sx > tx ? sx - tx : tx - sx;
    int dy = sy > ty ? sy - ty : ty - sy;
    if (dx > 58 || dy > 30)
        return SO_CONTINUE(int, navigation_distance_hook,
                           map, sx, sy, tx, ty, limit, first_direction);
    if (58 - dx < dx)
        dx = 58 - dx;
    if (30 - dy < dy)
        dy = 30 - dy;
    if (dx + dy > limit)
        return -1;

    /* Each map cell is enqueued at most once. Local storage avoids the
     * native vector/iterator work and needs no shared-cache invalidation. */
    struct { uint8_t x, y; } queue[MAP_CELLS];
    uint8_t visited[(MAP_CELLS + 7) / 8] = {0};
    unsigned initial = MAP_WIDTH * sy + sx;
    visited[initial >> 3] = 1u << (initial & 7);
    queue[0].x = sx;
    queue[0].y = sy;
    unsigned begin = 0, end = 1;
    for (int distance = 0; distance < limit && begin != end; ++distance) {
        unsigned layer_end = end;
        while (begin != layer_end) {
            int x = queue[begin].x, y = queue[begin].y;
            ++begin;
            for (int direction = 0; direction < 4; ++direction) {
                if (!distance && direction != first_direction)
                    continue;
                int nx = wrap_x(x + step_x[direction]);
                int ny = wrap_y(y + step_y[direction]);
                /* Native search accepts the destination before consulting
                 * visited bits or wall attributes, even for a blocked tile. */
                if (nx == tx && ny == ty)
                    return distance + 1;
                unsigned index = MAP_WIDTH * ny + nx;
                uint8_t bit = 1u << (index & 7);
                if (visited[index >> 3] & bit)
                    continue;
                visited[index >> 3] |= bit;
                uint8_t tile = ((const uint8_t *)map)[0xc08 + index];
                if (tile < sizeof(wall_attributes) && (wall_attributes[tile] & 1)) {
                    queue[end].x = nx;
                    queue[end].y = ny;
                    ++end;
                }
            }
        }
    }
    return -1;
}

/* The native return value is a C++ vector passed through an explicit hidden
 * first argument. Its allocator and ownership stay native so Aim's copies,
 * sorting, erases and destructors continue to work unchanged. */
static void navigation_turn_dirs(void *out, const void *map, float x, float y,
                                 int type, int excluded) {
    if (type < 0 || type > INT_MAX - 4) {
        so_hook_unpatch(&navigation_turn_hook);
        ((void (*)(void *, const void *, float, float, int, int))
            navigation_turn_hook.thumb_addr)(out, map, x, y, type, excluded);
        so_hook_repatch(&navigation_turn_hook);
        return;
    }
    int values[4];
    unsigned count = 0;
    int iy = navigation_map_y(map, y);
    int ix = navigation_map_x(map, x);
    for (unsigned i = 0; i < 4; ++i) {
        int direction = (type + (int)i) % 4;
        int nx = wrap_x(ix + step_x[direction]);
        int ny = wrap_y(iy + step_y[direction]);
        if (direction == excluded)
            continue;
        uint8_t tile = ((const uint8_t *)map)[0xc08 + MAP_WIDTH * ny + nx];
        if (navigation_wall_attribute(map, tile, type, direction) & 0x40)
            values[count++] = direction;
    }
    memset(out, 0, 12);
    if (count) {
        /* Match the native growth result (capacity 1, 2 or 4), allocating
         * only once instead of reallocating for each accepted direction. */
        navigation_vector_allocate(out, count == 3 ? 4 : count);
        void *begin = (void *)(uintptr_t)word(out, 0);
        memcpy(begin, values, count * sizeof(*values));
        uint32_t end = (uintptr_t)begin + count * sizeof(*values);
        memcpy((char *)out + 4, &end, sizeof(end));
    }
}

void game_navigation_install_hooks(void) {
    uintptr_t attr = game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer15GetElemWallAttrEhii", 0x140, 0xdfe6d339u);
    if (!attr)
        return;
    uintptr_t wall = game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer11GetWallAttrEffii", 0x50, 0x47eb7f05u);
    navigation_wall_elem = (void *)game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer11GetWallElemEff", 0x4c, 0x08d282d2u);
    uintptr_t distance = game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer10dis_less_nEiiiiii", 0x484, 0x7fa06b94u);
    uintptr_t turn = game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer18getGhostCanTurnDirEffii", 0x178, 0x2956bb83u);
    navigation_map_x = (void *)game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer7GetMapXEf", 0x4c, 0xa9984bf7u);
    navigation_map_y = (void *)game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer7GetMapYEf", 0x4c, 0x270552d1u);
    navigation_vector_allocate = (void *)game_patch_checked_function(
        "_ZNSt6__ndk16vectorIiNS_9allocatorIiEEE11__vallocateEj", 0x60, 0xf9dc636eu);
    uintptr_t dir_x = game_patch_checked_function(
        "_ZN9newPacman12cOnDirection7GetDirXEs", 0x54, 0x31eb7bc1u);
    uintptr_t dir_y = game_patch_checked_function(
        "_ZN9newPacman12cOnDirection7GetDirYEs", 0x54, 0xd52708fbu);
    if (distance) {
        uintptr_t code = distance & ~(uintptr_t)1;
        const void *steps = (void *)(code + 0x290 + word((void *)code, 0x464));
        static const int32_t expected[8] = {1, 0, 0, -1, -1, 0, 0, 1};
        if (memcmp(steps, expected, sizeof(expected)))
            distance = 0;
    }
    hook_addr(attr, (uintptr_t)navigation_wall_attribute);
    if (wall && navigation_wall_elem)
        hook_addr(wall, (uintptr_t)navigation_wall);
    if (distance)
        navigation_distance_hook = hook_addr(distance, (uintptr_t)navigation_distance);
    if (turn && navigation_map_x && navigation_map_y &&
        navigation_vector_allocate && dir_x && dir_y)
        navigation_turn_hook = hook_addr(turn, (uintptr_t)navigation_turn_dirs);
}
