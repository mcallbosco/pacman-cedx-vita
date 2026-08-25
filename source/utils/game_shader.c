#include "game_shader.h"
#include "game_patch.h"
#include <stdbool.h>
#include <string.h>

#pragma GCC optimize ("no-fast-math")

extern void *dlsym_soloader(void *handle, const char *symbol);

static void *(*shader_current)(void);
static void (*shader_set_current)(void *);
static void *(*shader_profile)(void);
static void (*shader_use_program)(uint32_t);
static int32_t (*shader_location)(uint32_t, const char *);
static void (*shader_matrix)(int32_t, int32_t, uint32_t, const float *);
static void (*shader_uniform1)(int32_t, int32_t);
/* Raw float bits retain the imported Android softfp ABI and NaN payloads. */
static void (*shader_uniform4)(int32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static void (*shader_attrib4)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static void (*shader_attrib2)(uint32_t, uint32_t, uint32_t);
static void (*shader_attrib1)(uint32_t, uint32_t);

enum { LOCATION_SLOTS = 32 };
static struct {
    uint32_t program;
    int32_t screen, diffuse;
    bool valid, diffuse_valid;
} locations[LOCATION_SLOTS];

static bool projection_valid;
static int32_t projection_width, projection_height;
static float projection[16] = {
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 1, 0,
    -1, 1, 0, 1
};

static uint32_t shader_word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

void game_shader_invalidate_program(uint32_t program) {
    size_t slot = program % LOCATION_SLOTS;
    if (locations[slot].program == program)
        locations[slot].valid = false;
}

static int32_t projection_location(uint32_t program, bool diffuse) {
    size_t slot = program % LOCATION_SLOTS;
    if (!locations[slot].valid || locations[slot].program != program) {
        locations[slot].program = program;
        locations[slot].screen = shader_location(program, "u_matScreen");
        locations[slot].valid = true;
        locations[slot].diffuse_valid = false;
    }
    if (!diffuse)
        return locations[slot].screen;
    if (!locations[slot].diffuse_valid) {
        locations[slot].diffuse = shader_location(program, "u_diffuseMap");
        locations[slot].diffuse_valid = true;
    }
    return locations[slot].diffuse;
}

static void update_projection(void) {
    const void *profile = shader_profile();
    int32_t width, height;
    memcpy(&width, (const char *)profile + 0x50, sizeof(width));
    memcpy(&height, (const char *)profile + 0x54, sizeof(height));
    if (!projection_valid || width != projection_width || height != projection_height) {
        projection[0] = 2.0f / (float)width;
        projection[5] = -2.0f / (float)height;
        projection_width = width;
        projection_height = height;
        projection_valid = true;
    }
}

static bool begin_shader(void *effect) {
    if (shader_word(effect, 0x8c) == UINT32_MAX)
        return false;
    if (shader_current() != effect) {
        shader_set_current(effect);
        shader_use_program(shader_word(effect, 0x8c));
    }
    return true;
}

static uint32_t apply_sprite_shader(void *effect, const void *sprite) {
    if (!begin_shader(effect))
        return 0;
    int32_t screen = projection_location(shader_word(effect, 0x8c), false);
    memcpy((char *)effect + 0x90, &screen, sizeof(screen));
    update_projection();
    /* Keep the setter: other effects can overwrite this uniform between
     * draws. vitaGL already skips the upload when its contents match. */
    shader_matrix(screen, 1, 0, projection);
    shader_attrib4(4, shader_word(sprite, 0x78), shader_word(sprite, 0x7c),
                    shader_word(sprite, 0x80), shader_word(sprite, 0x84));
    shader_uniform1(shader_word(effect, 0x98), 0);
    shader_uniform4(shader_word(effect, 0x9c),
                    shader_word(sprite, 0x88), shader_word(sprite, 0x8c),
                    shader_word(sprite, 0x90), shader_word(sprite, 0x94));
    return 1;
}

static uint32_t apply_default_shader(void *effect) {
    if (!begin_shader(effect))
        return 0;
    update_projection();
    uint32_t program = shader_word(effect, 0x8c);
    int32_t screen = projection_location(program, false);
    int32_t diffuse = projection_location(program, true);
    shader_matrix(screen, 1, 0, projection);
    const uint32_t one = 0x3f800000u;
    shader_attrib4(4, one, one, one, one);
    shader_attrib2(5, 0, 0);
    shader_attrib2(6, one, one);
    shader_attrib2(7, 0, 0);
    shader_attrib1(8, 0);
    shader_uniform1(diffuse, 0);
    return 1;
}

void game_shader_install_hooks(void) {
    uintptr_t sprite = game_patch_checked_function(
        "_ZN3sys8ShEffect5ApplyEPNS_7cSpriteE", 0x1c4, 0xa91519f9u);
    uintptr_t plain = game_patch_checked_function(
        "_ZN3sys8ShEffect5ApplyEv", 0x168, 0x02cc86f1u);
    shader_current = (void *)game_patch_checked_function(
        "_ZN3sys15ShEffectManager12GetCurEffectEv", 0x10, 0xac13d181u);
    shader_set_current = (void *)game_patch_checked_function(
        "_ZN3sys15ShEffectManager12SetCurEffectEPNS_12BaseShEffectE", 0x1c, 0xb9a8b7f5u);
    shader_profile = (void *)game_patch_checked_function(
        "_ZN3sys9SingletonINS_13DeviceProfileEE11GetInstanceEv", 0x48, 0x857a53beu);
    shader_use_program = dlsym_soloader(NULL, "glUseProgram");
    shader_location = dlsym_soloader(NULL, "glGetUniformLocation");
    shader_matrix = dlsym_soloader(NULL, "glUniformMatrix4fv");
    shader_uniform1 = dlsym_soloader(NULL, "glUniform1i");
    shader_uniform4 = dlsym_soloader(NULL, "glUniform4f");
    shader_attrib4 = dlsym_soloader(NULL, "glVertexAttrib4f");
    shader_attrib2 = dlsym_soloader(NULL, "glVertexAttrib2f");
    shader_attrib1 = dlsym_soloader(NULL, "glVertexAttrib1f");
    if (!shader_current || !shader_set_current || !shader_profile ||
        !shader_use_program || !shader_location || !shader_matrix ||
        !shader_uniform1 || !shader_uniform4 || !shader_attrib4 ||
        !shader_attrib2 || !shader_attrib1)
        return;
    if (sprite)
        hook_addr(sprite, (uintptr_t)apply_sprite_shader);
    if (plain)
        hook_addr(plain, (uintptr_t)apply_default_shader);
}
