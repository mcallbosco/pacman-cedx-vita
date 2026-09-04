#include "game_map_shader.h"
#include "game_map_effects.h"
#include "glutil.h"
#include "text_patch.h"
#include <stdlib.h>
#include <string.h>

static GLuint source_program, single_program;
static char *vertex_source, *fragment_source;
static int attempted;
static GLint source_matrix, source_hsv, source_sampler[2];
static GLint single_matrix, single_hsv, single_sampler;

void game_map_shader_invalidate(GLuint program) {
    if (program != source_program)
        return;
    if (single_program) {
        game_map_effects_invalidate(single_program);
        glDeleteProgram(single_program);
    }
    single_program = 0;
    source_program = 0;
    attempted = 0;
    free(vertex_source);
    free(fragment_source);
    vertex_source = fragment_source = NULL;
}

void game_map_shader_sources(GLuint program, const char *vertex, const char *fragment) {
    if (program != 5 || !vertex || !fragment ||
        (!strstr(vertex, "col.xyz *= a_ParamLight;") && !strstr(vertex, "col.xyz *= 1.0;")))
        return;
    const char *assignment = "v_oTexCoord2 = a_texCoordSub;";
    const char *sub = strstr(vertex, assignment);
    if (!sub)
        return;
    char *fs = pmcedx_single_texture_map_shader(fragment);
    if (!fs)
        return;
    char *single_effects = game_map_effects_single_shader(fs);
    if (single_effects) {
        free(fs);
        fs = single_effects;
    }
    char *vs = strdup(vertex);
    if (!vs) {
        free(fs);
        return;
    }
    /* Retain every other vertex/fragment operation from the installed shader.
     * Removing this assignment lets the compiler eliminate the second UV. */
    size_t offset = sub - vertex;
    memmove(vs + offset, vs + offset + strlen(assignment),
            strlen(vs + offset + strlen(assignment)) + 1);
    game_map_shader_invalidate(source_program);
    source_program = program;
    vertex_source = vs;
    fragment_source = fs;
}

static GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader)
        return 0;
    /* Use the same source-hash cache as the native shaders. */
    extern void load_shader(GLuint, const char *, size_t);
    load_shader(shader, source, strlen(source));
    glCompileShader_soloader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

void game_map_shader_prepare(void) {
    if (attempted || !source_program || !vertex_source || !fragment_source)
        return;
    attempted = 1;
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_source);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_source);
    GLuint program = vs && fs ? glCreateProgram() : 0;
    if (program) {
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        /* Share the map remap's indices so restoring its pointers restores
         * all attribute state changed by the specialized draw. */
        glBindAttribLocation(program, 0, "a_ParamLight");
        glBindAttribLocation(program, 1, "a_position");
        glBindAttribLocation(program, 2, "a_texCoord");
        glBindAttribLocation(program, 4, "a_color");
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok) {
            game_map_effects_register(program);
            source_matrix = glGetUniformLocation(source_program, "u_matScreen");
            source_hsv = glGetUniformLocation(source_program, "u_spDeltaHSV");
            source_sampler[0] = glGetUniformLocation(source_program, "u_diffuseMap2");
            source_sampler[1] = glGetUniformLocation(source_program, "u_diffuseMap");
            single_matrix = glGetUniformLocation(program, "u_matScreen");
            single_hsv = glGetUniformLocation(program, "u_spDeltaHSV");
            single_sampler = glGetUniformLocation(program, "u_diffuseMap");
            ok = ((source_matrix == -1) == (single_matrix == -1)) &&
                 source_sampler[0] != -1 && source_sampler[1] != -1 && single_sampler != -1 &&
                 ((source_hsv == -1) == (single_hsv == -1));
            /* Unknown uniforms or active attributes retain the native path. */
            GLint uniforms = 0, attributes = 0;
            glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniforms);
            glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &attributes);
            GLint light = glGetAttribLocation(program, "a_ParamLight");
            ok = ok && attributes == (light == -1 ? 3 : 4) &&
                 uniforms == 1 + (single_matrix != -1) + (single_hsv != -1) +
                    game_map_effects_uniform_count(program) &&
                 (light == -1 || light == 0) &&
                 glGetAttribLocation(program, "a_position") == 1 &&
                 glGetAttribLocation(program, "a_texCoord") == 2 &&
                 glGetAttribLocation(program, "a_color") == 4;
        }
        if (ok)
            single_program = program;
        else {
            game_map_effects_invalidate(program);
            glDeleteProgram(program);
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
}

static int endpoint(const uint32_t *vertices) {
    uint32_t alpha = vertices[9];
    int side = !(alpha & 0x7fffffffu) ? 0 : alpha == 0x3f800000u ? 1 : -1;
    if (side < 0)
        return -1;
    for (unsigned i = 1; i < 4; ++i) {
        alpha = vertices[i * 10 + 9];
        if (side ? alpha != 0x3f800000u : (alpha & 0x7fffffffu) != 0)
            return -1;
    }
    return side;
}

int game_map_shader_draw(GLuint program, GLfloat light, const void *vertices,
                         unsigned count) {
    if (!single_program || program != source_program || !count ||
        count > 64 * 4 || count % 4)
        return 0;
    const uint32_t *data = vertices;
    int side = endpoint(data);
    if (side < 0)
        return 0;
    /* Mixed strips retain one draw with the normal shader's endpoint sample
     * checks. Splitting them adds state changes and repacking during the wipe. */
    for (unsigned i = 1; i < count / 4; ++i)
        if (endpoint(data + i * 40) != side)
            return 0;
    if ((single_matrix != -1 && !vglCopyUniform(source_matrix, single_matrix)) ||
        (single_hsv != -1 && !vglCopyUniform(source_hsv, single_hsv)) ||
        !vglCopyUniform(source_sampler[1 - side], single_sampler) ||
        !vglCopyUniform(source_sampler[side], single_sampler))
        return 0;

    uint32_t compact[64 * 4 * 8];
    for (unsigned v = 0; v < count; ++v) {
        const uint32_t *src = data + v * 10;
        uint32_t *dst = compact + v * 8;
        memcpy(dst, src, 8);
        memcpy(dst + 2, src + (side ? 2 : 4), 8);
        memcpy(dst + 4, src + 6, 16);
    }
    glUseProgram(single_program);
    game_map_effects_apply(single_program);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, compact);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, compact + 2);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 32, compact + 4);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(4);
    glDisableVertexAttribArray(0);
    glVertexAttrib1f(0, light);
    glDrawArrays(GL_QUADS, 0, count);
    glUseProgram(program);
    return 1;
}
