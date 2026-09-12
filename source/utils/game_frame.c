#include "game_frame.h"
#include "game_patch.h"
#include <string.h>

static void set_word(void *sprite, size_t offset, uint32_t value) {
    memcpy((char *)sprite + offset, &value, sizeof(value));
}

static void set_frame(void *sprite, uint32_t frame, uint32_t unused) {
    set_word(sprite, 0x170, 1);
    set_word(sprite, 0xd4, frame);
}

static void set_frame_range(void *sprite, uint32_t frame, uint32_t first,
                            uint32_t last, uint32_t unused) {
    set_frame(sprite, frame, 0);
    set_word(sprite, 0xdc, first);
    set_word(sprite, 0xe0, last);
}

static void set_frame_position(void *sprite, uint32_t frame, uint32_t first,
                               uint32_t last, uint32_t x, uint32_t y, uint32_t mode) {
    set_frame_range(sprite, frame, first, last, 0);
    set_word(sprite, 0x48, mode);
    void *object = (char *)sprite + 12;
    void *vtable;
    void (*position)(void *, uint32_t, uint32_t);
    memcpy(&vtable, object, sizeof(vtable));
    memcpy(&position, (char *)vtable + 12, sizeof(position));
    position(object, x, y);
}

void game_frame_install_hooks(void) {
    uintptr_t frame = game_patch_checked_function(
        "_ZN3sys13MomongaSprite13SetPaintFrameEii", 0x2e, 0x33e070d4u);
    if (frame)
        hook_addr(frame, (uintptr_t)set_frame);
    frame = game_patch_checked_function(
        "_ZN3sys13MomongaSprite13SetPaintFrameEiiii", 0x4e, 0x47961d0bu);
    if (frame)
        hook_addr(frame, (uintptr_t)set_frame_range);
    frame = game_patch_checked_function(
        "_ZN3sys13MomongaSprite13SetPaintFrameEiiiffi", 0x74, 0xb6b96d31u);
    if (frame)
        hook_addr(frame, (uintptr_t)set_frame_position);
}
