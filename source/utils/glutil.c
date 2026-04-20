/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/settings.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>

#ifndef VITA_MSAA_MODE
#define VITA_MSAA_MODE SCE_GXM_MULTISAMPLE_NONE
#endif

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

#define MAX_DEBUG_SHADERS 128
#define MAX_DEBUG_PROGRAMS 64

typedef struct ShaderDebugInfo {
    GLenum type;
    char *source;
    size_t source_len;
} ShaderDebugInfo;

typedef struct ProgramDebugInfo {
    GLuint vertex_shader;
    GLuint fragment_shader;
} ProgramDebugInfo;

static ShaderDebugInfo shader_debug[MAX_DEBUG_SHADERS];
static ProgramDebugInfo program_debug[MAX_DEBUG_PROGRAMS];

static void free_tracked_shader_source(GLuint shader) {
    if (shader < MAX_DEBUG_SHADERS && shader_debug[shader].source) {
        free(shader_debug[shader].source);
        shader_debug[shader].source = NULL;
        shader_debug[shader].source_len = 0;
    }
}

static void track_shader_source(GLuint shader, const char *src, size_t len) {
    if (shader >= MAX_DEBUG_SHADERS || !src)
        return;

    free_tracked_shader_source(shader);
    shader_debug[shader].source = malloc(len + 1);
    if (!shader_debug[shader].source)
        return;

    memcpy(shader_debug[shader].source, src, len);
    shader_debug[shader].source[len] = '\0';
    shader_debug[shader].source_len = len;
}

static void dump_shader_source(GLuint shader) {
    if (shader >= MAX_DEBUG_SHADERS)
        return;

    ShaderDebugInfo *info = &shader_debug[shader];
    l_info("[SHDBG] shader=%u type=%s len=%u",
           shader,
           info->type == GL_VERTEX_SHADER ? "VERTEX" :
           info->type == GL_FRAGMENT_SHADER ? "FRAGMENT" : "UNKNOWN",
           (unsigned)info->source_len);
    if (info->source) {
        l_info("[SHSRC] shader=%u\n%s", shader, info->source);
    } else {
        l_info("[SHSRC] shader=%u source unavailable", shader);
    }
}

static void dump_shader_info_log(GLuint shader) {
    GLint log_len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
    if (log_len > 1) {
        char *log_buf = malloc(log_len + 1);
        if (!log_buf)
            return;
        glGetShaderInfoLog(shader, log_len, NULL, log_buf);
        log_buf[log_len] = '\0';
        l_info("[SHLOG] shader=%u\n%s", shader, log_buf);
        free(log_buf);
    }
}

static void dump_program_reflection(GLuint program) {
    GLint active_uniforms = 0, active_uniform_max = 0;
    GLint active_attribs = 0, active_attrib_max = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &active_uniforms);
    glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &active_uniform_max);
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &active_attribs);
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &active_attrib_max);
    l_info("[PREFL] prog=%u active_uniforms=%d active_attribs=%d", program, active_uniforms, active_attribs);

    if (active_uniforms > 0 && active_uniform_max > 0) {
        char *name = malloc((size_t)active_uniform_max + 1);
        if (name) {
            for (GLint i = 0; i < active_uniforms; i++) {
                GLsizei out_len = 0;
                GLint size = 0;
                GLenum type = 0;
                glGetActiveUniform(program, (GLuint)i, active_uniform_max, &out_len, &size, &type, name);
                name[out_len] = '\0';
                l_info("[PUNIF] prog=%u idx=%d name=\"%s\" size=%d type=0x%x loc=%d",
                       program, i, name, size, type, glGetUniformLocation(program, name));
            }
            free(name);
        }
    }

    if (active_attribs > 0 && active_attrib_max > 0) {
        char *name = malloc((size_t)active_attrib_max + 1);
        if (name) {
            for (GLint i = 0; i < active_attribs; i++) {
                GLsizei out_len = 0;
                GLint size = 0;
                GLenum type = 0;
                glGetActiveAttrib(program, (GLuint)i, active_attrib_max, &out_len, &size, &type, name);
                name[out_len] = '\0';
                l_info("[PATTR] prog=%u idx=%d name=\"%s\" size=%d type=0x%x loc=%d",
                       program, i, name, size, type, glGetAttribLocation(program, name));
            }
            free(name);
        }
    }
}

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

static int settings_msaa_to_gxm_mode(int msaa_mode) {
    switch (settings_sanitize_msaa_mode(msaa_mode)) {
        case SETTING_MSAA_OFF: return SCE_GXM_MULTISAMPLE_NONE;
        case SETTING_MSAA_2X:  return SCE_GXM_MULTISAMPLE_2X;
        case SETTING_MSAA_4X:  return SCE_GXM_MULTISAMPLE_4X;
        default:               return VITA_MSAA_MODE;
    }
}

void gl_init() {
    int gxm_msaa_mode = settings_msaa_to_gxm_mode(setting_msaaMode);
    l_info("gl_init: MSAA setting=%s (gxm=%d)", settings_msaa_to_string(setting_msaaMode), gxm_msaa_mode);
    vglInitExtended(0, 960, 544, 24 * 1024 * 1024, gxm_msaa_mode);
}

void gl_swap() {
    vglSwapBuffers(GL_FALSE);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    /* Strip Windows \r characters - vitashark/SceShaccCg can choke on them */
    size_t clean_len = 0;
    for (size_t i = 0; i < total_length; i++) {
        if (str[i] != '\r') {
            str[clean_len++] = str[i];
        }
    }
    str[clean_len] = '\0';
    track_shader_source(shader, str, clean_len);

    l_info("glShaderSource: shader=%u, len=%u, first4bytes=[%02x %02x %02x %02x]",
           shader, (unsigned)clean_len,
           (unsigned char)(str[0]), (unsigned char)(str[1]),
           (unsigned char)(str[2]), (unsigned char)(str[3]));
    /* Print first 80 chars of shader source */
    {
        char preview[81];
        unsigned plen = clean_len < 80 ? (unsigned)clean_len : 80;
        memcpy(preview, str, plen);
        preview[plen] = '\0';
        l_info("  src: %s", preview);
    }

    load_shader(shader, str, clean_len);

    free(str);
}

GLuint glCreateProgram_soloader(void) {
    GLuint prog = glCreateProgram();
    l_info("glCreateProgram() = %u", prog);
    if (prog < MAX_DEBUG_PROGRAMS) {
        program_debug[prog].vertex_shader = 0;
        program_debug[prog].fragment_shader = 0;
    }
    return prog;
}

GLuint glCreateShader_soloader(GLenum type) {
    GLuint shader = glCreateShader(type);
    l_info("glCreateShader(%s) = %u",
           type == GL_VERTEX_SHADER ? "VERTEX" :
           type == GL_FRAGMENT_SHADER ? "FRAGMENT" : "UNKNOWN", shader);
    if (shader < MAX_DEBUG_SHADERS)
        shader_debug[shader].type = type;
    return shader;
}

void glAttachShader_soloader(GLuint program, GLuint shader) {
    l_info("glAttachShader(prog=%u, shader=%u)", program, shader);
    if (program < MAX_DEBUG_PROGRAMS && shader < MAX_DEBUG_SHADERS) {
        if (shader_debug[shader].type == GL_VERTEX_SHADER)
            program_debug[program].vertex_shader = shader;
        else if (shader_debug[shader].type == GL_FRAGMENT_SHADER)
            program_debug[program].fragment_shader = shader;
    }
    glAttachShader(program, shader);
}

static int prog5_link_count = 0;
void glLinkProgram_soloader(GLuint program) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glLinkProgram<%p>(program: %i)\n", __builtin_return_address(0), program);
#endif
    if (program == 5) prog5_link_count++;
    l_info("glLinkProgram: linking program %u (link #%d for prog5)...",
           program, program == 5 ? prog5_link_count : 0);
    glLinkProgram(program);
    l_info("glLinkProgram: link call returned for program %u", program);
    if (program == 5 && program < MAX_DEBUG_PROGRAMS) {
        GLuint vs = program_debug[program].vertex_shader;
        GLuint fs = program_debug[program].fragment_shader;
        l_info("[PLINK] prog=%u attached_vs=%u attached_fs=%u", program, vs, fs);
        if (vs) {
            dump_shader_source(vs);
            dump_shader_info_log(vs);
        }
        if (fs) {
            dump_shader_source(fs);
            dump_shader_info_log(fs);
        }
    }
    /* After second link of prog 5, re-query u_matScreen to check if location changed */
    if (program == 5 && prog5_link_count == 2) {
        GLint loc_after = glGetUniformLocation(5, "u_matScreen");
        l_info("[RELINK] prog 5 re-linked. u_matScreen loc after 2nd link = %d (0x%x)",
               loc_after, (unsigned)loc_after);
    }

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        if (log_len > 0) {
            char *log_buf = malloc(log_len + 1);
            glGetProgramInfoLog(program, log_len, NULL, log_buf);
            l_error("Program %i link failed: %s", program, log_buf);
            free(log_buf);
        } else {
            l_error("Program %i link failed (no log)", program);
        }
    } else {
        l_info("Program %i linked OK", program);
        if (program == 5) {
            dump_program_reflection(program);
        }
    }
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        l_info("glCompileShader: compiling shader %u...", shader);
        glCompileShader(shader);
        l_info("glCompileShader: shader %u compile call returned", shader);

        /* Check compilation status before trying to serialize */
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_FALSE) {
            GLint log_len = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
            if (log_len > 0) {
                char *log_buf = malloc(log_len + 1);
                glGetShaderInfoLog(shader, log_len, NULL, log_buf);
                l_error("Shader %i compile failed: %s", shader, log_buf);
                free(log_buf);
            } else {
                l_error("Shader %i compile failed (no log)", shader);
            }
        } else if (shader < MAX_DEBUG_SHADERS && shader_debug[shader].type && shader <= 10) {
            dump_shader_info_log(shader);
        }
#ifdef DUMP_COMPILED_SHADERS
        else {
            if (next_shader_fname[0] != '\0') {
                void *bin = vglMalloc(32 * 1024);
                if (bin) {
                    GLsizei len = 0;
                    vglGetShaderBinary(shader, 32 * 1024, &len, bin);
                    GLenum err = glGetError();
                    if (err == GL_NO_ERROR && len > 0) {
                        file_save(next_shader_fname, bin, len);
                    } else {
                        l_warn("Skipping shader cache write: shader=%u err=0x%x len=%d path=%s",
                               shader, (unsigned)err, (int)len, next_shader_fname);
                    }
                    vglFree(bin);
                } else {
                    l_warn("Skipping shader cache write: vglMalloc failed for shader=%u", shader);
                }
                next_shader_fname[0] = '\0';
            }
        }
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    l_info("load_shader: computing SHA1...");
    char* sha_name = str_sha1sum(string, length);
    l_info("load_shader: sha=%s", sha_name);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), SHADER_CACHE_PATH "%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        l_info("load_shader: loading cached GXP %s", gxp_path);
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);
        l_info("load_shader: file_load done, buffer=%p size=%u", buffer, (unsigned)size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);
        l_info("load_shader: glShaderBinary done");

        free(buffer);
        skip_next_compile = GL_TRUE;
        next_shader_fname[0] = '\0';
        l_info("load_shader: cached GXP loaded OK");
    } else {
        GLint gl_length = (GLint)length;
        l_info("load_shader: calling glShaderSource (len=%d)...", gl_length);
        glShaderSource(shader, 1, &string, &gl_length);
        strcpy(next_shader_fname, gxp_path);
        l_info("load_shader: glShaderSource done");
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    /* gxp_path is a writable cache; cg_path is read-only source shipped in data/. */
    snprintf(gxp_path, sizeof(gxp_path), SHADER_CACHE_PATH "%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH "cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), "ux0:data/pacmancedx/glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), "ux0:data/pacmancedx/glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
