/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 * Copyright (C) 2026      Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  dynlib.c
 * @brief Resolving dynamic imports of the .so.
 */

#include <psp2/kernel/clib.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <netdb.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include <locale.h>
#include <poll.h>

#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <so_util/so_util.h>
#include <utime.h>

#include "utils/glutil.h"
#include "utils/utils.h"
#include "utils/logger.h"

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "reimpl/errno.h"
#include "reimpl/io.h"
#include "reimpl/log.h"
#include "reimpl/mem.h"
#include "reimpl/pthr.h"
#include "reimpl/sys.h"
#include "reimpl/egl.h"
#include "reimpl/time64.h"
#include "reimpl/asset_manager.h"

const unsigned int __page_size = PAGE_SIZE;

extern void * _ZNSt9exceptionD2Ev;
extern void * _ZSt17__throw_bad_allocv;
extern void * _ZSt9terminatev;
extern void * _ZdaPv;
extern void * _ZdlPv;
extern void * _Znaj;
extern void * __cxa_allocate_exception;
extern void * __cxa_begin_catch;
extern void * __cxa_end_catch;
extern void * __cxa_free_exception;
extern void * __cxa_rethrow;
extern void * __cxa_throw;
extern void * __gxx_personality_v0;
extern void *_ZNSt8bad_castD1Ev;
extern void *_ZTISt8bad_cast;
extern void *_ZTISt9exception;
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE;
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE;
extern void *_Znwj;
extern void *__aeabi_atexit;
extern void *__aeabi_d2lz;
extern void *__aeabi_d2ulz;
extern void *__aeabi_dadd;
extern void *__aeabi_dcmpgt;
extern void *__aeabi_dcmplt;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_f2lz;
extern void *__aeabi_f2ulz;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ldivmod;
extern void *__aeabi_memclr;
extern void *__aeabi_memcpy;
extern void *__aeabi_memmove;
extern void *__aeabi_memset4;
extern void *__aeabi_memset8;
extern void *__aeabi_memset;
extern void *__aeabi_ui2d;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;
extern void *__aeabi_unwind_cpp_pr0;
extern void *__aeabi_unwind_cpp_pr1;
extern void *__cxa_atexit;
extern void *__cxa_call_unexpected;
extern void *__cxa_finalize;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__gnu_ldivmod_helper;
extern void *__gnu_unwind_frame;
extern void *__srget;
extern void *__stack_chk_guard;
extern void *__swbuf;

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static FILE __sF_fake[3];

/* timer_create stub: write a dummy timer ID so the caller doesn't use garbage */
#include <signal.h>
typedef void *timer_t_bionic;
static int timer_create_soloader(int clockid, void *sevp, timer_t_bionic *timerid) {
    (void)clockid; (void)sevp;
    l_warn("timer_create(%d, %p, %p): stubbed", clockid, sevp, timerid);
    if (timerid) *timerid = (timer_t_bionic)0x1; /* dummy non-NULL timer ID */
    return 0;
}

static int timer_settime_soloader(timer_t_bionic timerid, int flags, const void *new_value, void *old_value) {
    (void)timerid; (void)flags; (void)new_value; (void)old_value;
    return 0;
}

static int timer_delete_soloader(timer_t_bionic timerid) {
    (void)timerid;
    return 0;
}

static char dlerror_buf[192];
static int dlerror_pending = 0;

static void dlerror_clear_soloader(void) {
    dlerror_buf[0] = '\0';
    dlerror_pending = 0;
}

static void dlerror_set_soloader(const char *msg) {
    if (!msg) msg = "dynamic loader error";
    sceClibSnprintf(dlerror_buf, sizeof(dlerror_buf), "%s", msg);
    dlerror_pending = 1;
}

static void *dlopen_soloader(const char *filename, int flag) {
    (void)filename;
    (void)flag;
    dlerror_clear_soloader();
    return (void *)0x1;
}

static int dlclose_soloader(void *handle) {
    (void)handle;
    dlerror_clear_soloader();
    return 0;
}

static const char *dlerror_soloader(void) {
    if (!dlerror_pending)
        return NULL;
    dlerror_pending = 0;
    return dlerror_buf;
}

void *dlsym_soloader(void * handle, const char * symbol);

/*
 * Soft-float ↔ hard-float ABI bridge wrappers.
 *
 * The Android .so is compiled with -mfloat-abi=softfp (floats passed in
 * integer registers r0-r3), but VitaSDK uses -mfloat-abi=hard (floats
 * passed in VFP registers s0-s15 / d0-d7). Every math function that
 * takes or returns float/double needs a wrapper to shuffle registers.
 */

/* float func(float) — e.g. sinf, cosf, sqrtf */
#define WRAP_FF(name) \
    __attribute__((naked)) static void name##_sfp(void) { \
        __asm__ volatile( \
            "vmov s0, r0\n" \
            "push {lr}\n" \
            "bl " #name "\n" \
            "vmov r0, s0\n" \
            "pop {pc}\n" \
        ); \
    }

/* double func(double) — e.g. sin, cos, sqrt */
#define WRAP_DD(name) \
    __attribute__((naked)) static void name##_sfp(void) { \
        __asm__ volatile( \
            "vmov d0, r0, r1\n" \
            "push {lr}\n" \
            "bl " #name "\n" \
            "vmov r0, r1, d0\n" \
            "pop {pc}\n" \
        ); \
    }

/* float func(float, float) — e.g. atan2f, powf, fmodf, fmaxf, fminf */
#define WRAP_FFF(name) \
    __attribute__((naked)) static void name##_sfp(void) { \
        __asm__ volatile( \
            "vmov s0, r0\n" \
            "vmov s1, r1\n" \
            "push {lr}\n" \
            "bl " #name "\n" \
            "vmov r0, s0\n" \
            "pop {pc}\n" \
        ); \
    }

/* double func(double, double) — e.g. atan2, pow, fmod, fmax, fmin */
#define WRAP_DDD(name) \
    __attribute__((naked)) static void name##_sfp(void) { \
        __asm__ volatile( \
            "vmov d0, r0, r1\n" \
            "vmov d1, r2, r3\n" \
            "push {lr}\n" \
            "bl " #name "\n" \
            "vmov r0, r1, d0\n" \
            "pop {pc}\n" \
        ); \
    }

/* void func(float, float*, float*) — sincosf */
__attribute__((naked)) static void sincosf_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"    /* float arg: r0 -> s0 */
        "mov r0, r1\n"     /* sinp: r1 -> r0 */
        "mov r1, r2\n"     /* cosp: r2 -> r1 */
        "b sincosf\n"      /* tail-call (void return) */
    );
}

/* void func(double, double*, double*) — sincos */
__attribute__((naked)) static void sincos_sfp(void) {
    __asm__ volatile(
        "vmov d0, r0, r1\n"  /* double arg: r0:r1 -> d0 */
        "mov r0, r2\n"       /* sinp: r2 -> r0 */
        "mov r1, r3\n"       /* cosp: r3 -> r1 */
        "b sincos\n"         /* tail-call (void return) */
    );
}

/* float func(float, int*) — frexpf: float in r0, int* in r1 */
__attribute__((naked)) static void frexpf_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"     /* float arg: r0 -> s0 */
        "mov r0, r1\n"      /* int* exp: r1 -> r0 */
        "push {lr}\n"
        "bl frexpf\n"
        "vmov r0, s0\n"
        "pop {pc}\n"
    );
}

/* double func(double, int*) — frexp: double in r0:r1, int* in r2 */
__attribute__((naked)) static void frexp_sfp(void) {
    __asm__ volatile(
        "vmov d0, r0, r1\n"  /* double arg: r0:r1 -> d0 */
        "mov r0, r2\n"       /* int* exp: r2 -> r0 */
        "push {lr}\n"
        "bl frexp\n"
        "vmov r0, r1, d0\n"
        "pop {pc}\n"
    );
}

/* float func(float, int) — ldexpf, scalbnf: float in r0, int in r1 */
#define WRAP_FFI(name) \
    __attribute__((naked)) static void name##_sfp(void) { \
        __asm__ volatile( \
            "vmov s0, r0\n" \
            "mov r0, r1\n" \
            "push {lr}\n" \
            "bl " #name "\n" \
            "vmov r0, s0\n" \
            "pop {pc}\n" \
        ); \
    }

/* double func(double, int) — ldexp, scalbn: double in r0:r1, int in r2 */
#define WRAP_DDI(name) \
    __attribute__((naked)) static void name##_sfp(void) { \
        __asm__ volatile( \
            "vmov d0, r0, r1\n" \
            "mov r0, r2\n" \
            "push {lr}\n" \
            "bl " #name "\n" \
            "vmov r0, r1, d0\n" \
            "pop {pc}\n" \
        ); \
    }

/* long func(float) — lroundf, lrintf: float in r0, returns long in r0 */
#define WRAP_LF(name) \
    __attribute__((naked)) static void name##_sfp(void) { \
        __asm__ volatile( \
            "vmov s0, r0\n" \
            "b " #name "\n" \
        ); \
    }

/* long func(double) — lround, lrint: double in r0:r1, returns long in r0 */
#define WRAP_LD(name) \
    __attribute__((naked)) static void name##_sfp(void) { \
        __asm__ volatile( \
            "vmov d0, r0, r1\n" \
            "b " #name "\n" \
        ); \
    }

/* double func(double, double*) — modf: double in r0:r1, double* in r2 */
__attribute__((naked)) static void modf_sfp(void) {
    __asm__ volatile(
        "vmov d0, r0, r1\n"
        "mov r0, r2\n"
        "push {lr}\n"
        "bl modf\n"
        "vmov r0, r1, d0\n"
        "pop {pc}\n"
    );
}

/* atof(const char*) -> double: args fine (pointer in r0), return d0 -> r0:r1 */
__attribute__((naked)) static void atof_sfp(void) {
    __asm__ volatile(
        "push {lr}\n"
        "bl atof\n"
        "vmov r0, r1, d0\n"
        "pop {pc}\n"
    );
}

/* strtod(const char*, char**) -> double: args fine, return d0 -> r0:r1 */
__attribute__((naked)) static void strtod_sfp(void) {
    __asm__ volatile(
        "push {lr}\n"
        "bl strtod\n"
        "vmov r0, r1, d0\n"
        "pop {pc}\n"
    );
}

/* strtof(const char*, char**) -> float: args fine, return s0 -> r0 */
__attribute__((naked)) static void strtof_sfp(void) {
    __asm__ volatile(
        "push {lr}\n"
        "bl strtof\n"
        "vmov r0, s0\n"
        "pop {pc}\n"
    );
}

/* difftime(time_t, time_t) -> double: on 32-bit ARM, time_t is 32-bit int,
 * so args are in r0, r1. Return d0 -> r0:r1 */
__attribute__((naked)) static void difftime_sfp(void) {
    __asm__ volatile(
        "push {lr}\n"
        "bl difftime\n"
        "vmov r0, r1, d0\n"
        "pop {pc}\n"
    );
}

/* strtold(const char*, char**) -> long double: on ARM, long double = double.
 * Args fine, return d0 -> r0:r1 */
__attribute__((naked)) static void strtold_sfp(void) {
    __asm__ volatile(
        "push {lr}\n"
        "bl strtold\n"
        "vmov r0, r1, d0\n"
        "pop {pc}\n"
    );
}

/*
 * OpenGL soft-float ABI wrappers.
 * GL functions taking float args are called from the Android .so (softfp)
 * but vitaGL expects hard-float. We need wrappers for each signature.
 */

/* void func(GLint loc, GLfloat v0) — glUniform1f */
__attribute__((naked)) static void glUniform1f_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"       /* float v0: r1 -> s0 */
        "b glUniform1f\n"
    );
}

/* void func(GLint loc, GLfloat v0, GLfloat v1) — glUniform2f */
__attribute__((naked)) static void glUniform2f_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"       /* v0: r1 -> s0 */
        "vmov s1, r2\n"       /* v1: r2 -> s1 */
        "b glUniform2f\n"
    );
}

/* void func(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2) — glUniform3f */
__attribute__((naked)) static void glUniform3f_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"
        "vmov s1, r2\n"
        "vmov s2, r3\n"
        "b glUniform3f\n"
    );
}

/* void func(GLint loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) — glUniform4f
 * 5 args: loc in r0, v0-v2 in r1-r3, v3 on stack [sp] */
__attribute__((naked)) static void glUniform4f_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"
        "vmov s1, r2\n"
        "vmov s2, r3\n"
        "ldr r1, [sp]\n"      /* v3 from stack */
        "vmov s3, r1\n"
        "b glUniform4f\n"
    );
}

/* void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) — 4 floats in r0-r3 */
__attribute__((naked)) static void glClearColor_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "vmov s2, r2\n"
        "vmov s3, r3\n"
        "b glClearColor\n"
    );
}

/* void glClearDepthf(GLfloat depth) — 1 float in r0 */
__attribute__((naked)) static void glClearDepthf_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "b glClearDepthf\n"
    );
}

/* void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) — 4 floats in r0-r3 */
__attribute__((naked)) static void glColor4f_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "vmov s2, r2\n"
        "vmov s3, r3\n"
        "b glColor4f\n"
    );
}

/* void glDepthRangef(GLfloat near, GLfloat far) — 2 floats in r0, r1 */
__attribute__((naked)) static void glDepthRangef_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "b glDepthRangef\n"
    );
}

/* void glLineWidth(GLfloat width) — 1 float in r0 */
__attribute__((naked)) static void glLineWidth_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "b glLineWidth\n"
    );
}

/* void glPolygonOffset(GLfloat factor, GLfloat units) — 2 floats */
__attribute__((naked)) static void glPolygonOffset_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "b glPolygonOffset\n"
    );
}

/* glSampleCoverage — not exported by vitaGL, stubbed as ret0 in table */

/* void glTexEnvf(GLenum target, GLenum pname, GLfloat param) — int, int, float in r2 */
__attribute__((naked)) static void glTexEnvf_sfp(void) {
    __asm__ volatile(
        "vmov s0, r2\n"
        "b glTexEnvf\n"
    );
}

/* void glVertexAttrib1f(GLuint idx, GLfloat v0) */
__attribute__((naked)) static void glVertexAttrib1f_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"
        "b glVertexAttrib1f\n"
    );
}

static float prog5_paramlight_value = 1.0f;

typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *pointer;
    GLboolean normalized;
} VAPStateFix;

static VAPStateFix vap_state_fix[16] = {0};
static GLuint gl_current_program_fix = 0;

static inline void apply_prog5_attr_remap_fix(void) {
    // Program 5 attribute locations on Vita are observed as:
    // 0=a_ParamLight, 1=a_position, 2=a_texCoord, 3=a_texCoordSub, 4=a_color.
    // The game feeds position/uv/color/light as indices 0/1/2/10, so remap here.
    if (vap_state_fix[0].pointer) {
        glVertexAttribPointer(1, vap_state_fix[0].size, vap_state_fix[0].type,
                              vap_state_fix[0].normalized, vap_state_fix[0].stride, vap_state_fix[0].pointer);
        glEnableVertexAttribArray(1);
    }
    if (vap_state_fix[1].pointer) {
        glVertexAttribPointer(2, vap_state_fix[1].size, vap_state_fix[1].type,
                              vap_state_fix[1].normalized, vap_state_fix[1].stride, vap_state_fix[1].pointer);
        glEnableVertexAttribArray(2);
    }
    if (vap_state_fix[3].pointer) {
        glVertexAttribPointer(3, vap_state_fix[3].size, vap_state_fix[3].type,
                              vap_state_fix[3].normalized, vap_state_fix[3].stride, vap_state_fix[3].pointer);
        glEnableVertexAttribArray(3);
    }
    if (vap_state_fix[2].pointer) {
        glVertexAttribPointer(4, vap_state_fix[2].size, vap_state_fix[2].type,
                              vap_state_fix[2].normalized, vap_state_fix[2].stride, vap_state_fix[2].pointer);
        glEnableVertexAttribArray(4);
    }
    glDisableVertexAttribArray(0);
    glVertexAttrib1f(0, prog5_paramlight_value);
}

static void glUseProgram_fix(GLuint program) {
    gl_current_program_fix = program;
    glUseProgram(program);
}

static void glVertexAttribPointer_fix(GLuint index, GLint size, GLenum type,
                                      GLboolean normalized, GLsizei stride,
                                      const void *pointer) {
    if (index < 16) {
        vap_state_fix[index].size = size;
        vap_state_fix[index].type = type;
        vap_state_fix[index].stride = stride;
        vap_state_fix[index].pointer = pointer;
        vap_state_fix[index].normalized = normalized;
    }
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

static void glDrawArrays_fix(GLenum mode, GLint first, GLsizei count) {
    if (gl_current_program_fix == 5) {
        apply_prog5_attr_remap_fix();
    }
    glDrawArrays(mode, first, count);
}

/* Soft-float ABI bridge + prog5 a_ParamLight fix (non-debug path). */
static void glVertexAttrib1f_sfp_fix(uint32_t idx, uint32_t float_bits) {
    float val;
    memcpy(&val, &float_bits, 4);
    glVertexAttrib1f(idx, val);

    if (gl_current_program_fix == 5 && idx == 10) {
        prog5_paramlight_value = val;
        glDisableVertexAttribArray(0);
        glVertexAttrib1f(0, val);
    }
}

/* Logging version: called from soft-float .so, idx=r0, float bits=r1 */
static int va1f_log_count = 0;
static void glVertexAttrib1f_sfp_log(uint32_t idx, uint32_t float_bits) {
    float val;
    memcpy(&val, &float_bits, 4);
    /* Always log high indices (a_ParamLight=10, a_mapoffset=9) */
    if (idx >= 9 || va1f_log_count < 5) {
        va1f_log_count++;
        l_info("[gl_dbg] glVertexAttrib1f(idx=%u, val=%f [0x%08x])", idx, val, float_bits);
    }
    glVertexAttrib1f(idx, val);
    if (gl_current_program_fix == 5 && idx == 10) {
        prog5_paramlight_value = val;
        l_info("[ATTRFIX] prog=5 attrib10 -> attrib0 val=%f", val);
        glDisableVertexAttribArray(0);
        glVertexAttrib1f(0, val);
    }
}

/* void glVertexAttrib2f(GLuint idx, GLfloat v0, GLfloat v1) */
__attribute__((naked)) static void glVertexAttrib2f_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"
        "vmov s1, r2\n"
        "b glVertexAttrib2f\n"
    );
}

/* void glVertexAttrib2f(GLuint idx, GLfloat v0, GLfloat v1) — logging version */
static int va2f_log_count = 0;
static void glVertexAttrib2f_sfp_log(uint32_t idx, uint32_t fb0, uint32_t fb1) {
    float v0, v1;
    memcpy(&v0, &fb0, 4);
    memcpy(&v1, &fb1, 4);
    if (va2f_log_count < 20) {
        va2f_log_count++;
        l_info("[gl_dbg] glVertexAttrib2f(idx=%u, %f, %f)", idx, v0, v1);
    }
    glVertexAttrib2f(idx, v0, v1);
}

/* void glVertexAttrib4f — logging version */
static int va4f_log_count = 0;
static void glVertexAttrib4f_sfp_log(uint32_t idx, uint32_t fb0, uint32_t fb1, uint32_t fb2, uint32_t fb3) {
    float v0, v1, v2, v3;
    memcpy(&v0, &fb0, 4);
    memcpy(&v1, &fb1, 4);
    memcpy(&v2, &fb2, 4);
    /* fb3 is on the stack in soft-float ABI — but this C function gets it as 5th arg which
     * on ARM goes on the stack. So it should be correct. */
    memcpy(&v3, &fb3, 4);
    if (va4f_log_count < 20) {
        va4f_log_count++;
        l_info("[gl_dbg] glVertexAttrib4f(idx=%u, %f, %f, %f, %f)", idx, v0, v1, v2, v3);
    }
    glVertexAttrib4f(idx, v0, v1, v2, v3);
}

/* void glVertexAttrib4f(GLuint idx, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
 * 5 args: idx in r0, v0-v2 in r1-r3, v3 on stack */
__attribute__((naked)) static void glVertexAttrib4f_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"
        "vmov s1, r2\n"
        "vmov s2, r3\n"
        "ldr r1, [sp]\n"
        "vmov s3, r1\n"
        "b glVertexAttrib4f\n"
    );
}

/* void glAlphaFunc(GLenum func, GLfloat ref) — int, float in r1 */
__attribute__((naked)) static void glAlphaFunc_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"
        "b glAlphaFunc\n"
    );
}

/* void glTexParameterf(GLenum target, GLenum pname, GLfloat param) */
__attribute__((naked)) static void glTexParameterf_sfp(void) {
    __asm__ volatile(
        "vmov s0, r2\n"
        "b glTexParameterf\n"
    );
}

/* void glPointSize(GLfloat size) */
__attribute__((naked)) static void glPointSize_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "b glPointSize\n"
    );
}

/* void glFogf(GLenum pname, GLfloat param) — int in r0, float in r1 */
__attribute__((naked)) static void glFogf_sfp(void) {
    __asm__ volatile(
        "vmov s0, r1\n"
        "b glFogf\n"
    );
}

/* void glTranslatef(GLfloat x, GLfloat y, GLfloat z) — 3 floats */
__attribute__((naked)) static void glTranslatef_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "vmov s2, r2\n"
        "b glTranslatef\n"
    );
}

/* glBlendColor — not exported by vitaGL, stubbed as ret0 in table */

/* void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) — 4 floats */
__attribute__((naked)) static void glRotatef_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "vmov s2, r2\n"
        "vmov s3, r3\n"
        "b glRotatef\n"
    );
}

/* void glScalef(GLfloat x, GLfloat y, GLfloat z) — 3 floats */
__attribute__((naked)) static void glScalef_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "vmov s2, r2\n"
        "b glScalef\n"
    );
}

/* void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
 * — 6 floats: r0-r3 + stack[0], stack[1] */
__attribute__((naked)) static void glOrthof_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "vmov s2, r2\n"
        "vmov s3, r3\n"
        "ldr r0, [sp, #0]\n"
        "ldr r1, [sp, #4]\n"
        "vmov s4, r0\n"
        "vmov s5, r1\n"
        "b glOrthof\n"
    );
}

/* void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
 * — 6 floats: r0-r3 + stack[0], stack[1] */
__attribute__((naked)) static void glFrustumf_sfp(void) {
    __asm__ volatile(
        "vmov s0, r0\n"
        "vmov s1, r1\n"
        "vmov s2, r2\n"
        "vmov s3, r3\n"
        "ldr r0, [sp, #0]\n"
        "ldr r1, [sp, #4]\n"
        "vmov s4, r0\n"
        "vmov s5, r1\n"
        "b glFrustumf\n"
    );
}

/* Generate all wrappers */

/* float func(float) */
WRAP_FF(acosf)  WRAP_FF(asinf)  WRAP_FF(atanf)
WRAP_FF(ceilf)  WRAP_FF(cosf)   WRAP_FF(exp2f)
WRAP_FF(expf)   WRAP_FF(floorf) WRAP_FF(log10f)
WRAP_FF(logf)   WRAP_FF(rintf)  WRAP_FF(roundf)
WRAP_FF(sinf)   WRAP_FF(sinhf)  WRAP_FF(sqrtf)
WRAP_FF(tanf)   WRAP_FF(truncf)

/* double func(double) */
WRAP_DD(acos)  WRAP_DD(asin)  WRAP_DD(atan)
WRAP_DD(ceil)  WRAP_DD(cos)   WRAP_DD(exp)
WRAP_DD(exp2)  WRAP_DD(floor) WRAP_DD(log)
WRAP_DD(log10) WRAP_DD(rint)  WRAP_DD(round)
WRAP_DD(sin)   WRAP_DD(sinh)  WRAP_DD(sqrt)
WRAP_DD(tan)   WRAP_DD(tanh)  WRAP_DD(trunc)

/* float func(float, float) */
WRAP_FFF(atan2f) WRAP_FFF(fmodf) WRAP_FFF(fmaxf)
WRAP_FFF(fminf)  WRAP_FFF(powf)

/* double func(double, double) */
WRAP_DDD(atan2) WRAP_DDD(fmod) WRAP_DDD(fmax)
WRAP_DDD(fmin)  WRAP_DDD(pow)

/* float/double func(val, int) */
WRAP_FFI(ldexpf)  WRAP_FFI(scalbnf)
WRAP_DDI(ldexp)   WRAP_DDI(scalbn)

/* long func(float/double) */
WRAP_LF(lroundf) WRAP_LF(lrintf)
WRAP_LD(lround)  WRAP_LD(lrint)


/* Debug: GL call counters per frame */
#ifdef DEBUG_OPENGL
int gl_draw_count = 0;
int gl_useprogram_count = 0;
int gl_clear_count = 0;
int gl_dbg_frame = 0;  /* set from main.c each frame */
static GLuint gl_current_program = 0;
static GLenum gl_active_texture_unit = GL_TEXTURE0;
static GLuint gl_bound_framebuffer = 0;

int frame_seq_logged = 0;
int frame_seq_target = -1;
static int prog5_state_dumped = 0;
static int maze_attr_dumped[8] = {0};
static int maze_mat4_dumped[8] = {0};
static int drawctx_prog1_log_count = 0;
static int drawctx_prog5_log_count = 0;
static int prog1_skip_log_count = 0;

/* Forward-declare VAP tracking for use in glDrawArrays_hook */
typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *pointer;
    GLboolean normalized;
} VAPState;
static VAPState vap_state[16] = {0};
static int prog5_vap_dumped = 0;

static int is_maze_program(GLuint program) {
    return program == 2 || program == 5;
}

static void dump_vertex_attrib_state(GLuint program) {
    for (GLuint i = 0; i <= 12; i++) {
        GLint enabled = 0, size = 0, stride = 0, type = 0, buffer = 0;
        GLfloat cur[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        if (i != 0) {
            glGetVertexAttribfv(i, GL_CURRENT_VERTEX_ATTRIB, cur);
        }
        if (enabled || i == 5 || i == 10 || i == 11 || i == 12) {
            l_info("[ATTR] prog=%u idx=%u enabled=%d size=%d type=0x%x stride=%d vbo=%d cur=(%f,%f,%f,%f)",
                   program, i, enabled, size, type, stride, buffer,
                   cur[0], cur[1], cur[2], cur[3]);
        }
    }
}

static void glDrawArrays_hook(GLenum mode, GLint first, GLsizei count) {
    gl_draw_count++;
    if (gl_current_program == 5 && frame_seq_target < 0)
        frame_seq_target = gl_dbg_frame;

    // Program 5 attribute locations on Vita are observed as:
    // 0=a_ParamLight, 1=a_position, 2=a_texCoord, 3=a_texCoordSub, 4=a_color.
    // The game feeds position/uv/color/light as indices 0/1/2/10, so remap here.
    if (gl_current_program == 5) {
        if (vap_state[0].pointer) {
            glVertexAttribPointer(1, vap_state[0].size, vap_state[0].type,
                                  vap_state[0].normalized, vap_state[0].stride, vap_state[0].pointer);
            glEnableVertexAttribArray(1);
        }
        if (vap_state[1].pointer) {
            glVertexAttribPointer(2, vap_state[1].size, vap_state[1].type,
                                  vap_state[1].normalized, vap_state[1].stride, vap_state[1].pointer);
            glEnableVertexAttribArray(2);
        }
        if (vap_state[3].pointer) {
            glVertexAttribPointer(3, vap_state[3].size, vap_state[3].type,
                                  vap_state[3].normalized, vap_state[3].stride, vap_state[3].pointer);
            glEnableVertexAttribArray(3);
        }
        if (vap_state[2].pointer) {
            glVertexAttribPointer(4, vap_state[2].size, vap_state[2].type,
                                  vap_state[2].normalized, vap_state[2].stride, vap_state[2].pointer);
            glEnableVertexAttribArray(4);
        }
        glDisableVertexAttribArray(0);
        glVertexAttrib1f(0, prog5_paramlight_value);
    }

    if ((gl_current_program == 5 && drawctx_prog5_log_count < 1200) ||
        (gl_current_program == 1 && drawctx_prog1_log_count < 600)) {
        if (gl_current_program == 5) drawctx_prog5_log_count++;
        else drawctx_prog1_log_count++;
        l_info("[DRAWCTX] f%d prog=%u mode=0x%x first=%d count=%d fb=%u",
               gl_dbg_frame, gl_current_program, mode, first, count, gl_bound_framebuffer);
    }
    if (gl_dbg_frame == frame_seq_target && !frame_seq_logged)
        l_info("[seq] f%d DrawArrays(mode=%x, count=%d, prog=%d)", gl_dbg_frame, mode, count, gl_current_program);
    /* Full state dump before first prog 5 draw */
    if (gl_current_program == 5 && !prog5_state_dumped) {
        prog5_state_dumped = 1;
        GLint vp[4], scissor[4];
        GLboolean cmask[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        l_info("[STATE] vp=%d,%d,%d,%d sci=%d,%d,%d,%d",
               vp[0], vp[1], vp[2], vp[3], scissor[0], scissor[1], scissor[2], scissor[3]);
        l_info("[STATE] BLEND=%d DEPTH=%d CULL=%d SCISSOR=%d STENCIL=%d",
               glIsEnabled(GL_BLEND), glIsEnabled(GL_DEPTH_TEST),
               glIsEnabled(GL_CULL_FACE), glIsEnabled(GL_SCISSOR_TEST),
               glIsEnabled(GL_STENCIL_TEST));
        glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
        l_info("[STATE] colorMask=%d,%d,%d,%d", cmask[0], cmask[1], cmask[2], cmask[3]);
        GLint front_face, cull_mode, vbo;
        glGetIntegerv(GL_FRONT_FACE, &front_face);
        glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
        l_info("[STATE] frontFace=0x%x cullMode=0x%x VBO=%d", front_face, cull_mode, vbo);
    }
    if (is_maze_program(gl_current_program) && !maze_attr_dumped[gl_current_program]) {
        maze_attr_dumped[gl_current_program] = 1;
        dump_vertex_attrib_state(gl_current_program);
    }
    /* Skip prog 1 draws for 60 frames after maze first appears */
    if (gl_current_program == 1 && frame_seq_target >= 0 &&
        gl_dbg_frame >= frame_seq_target && gl_dbg_frame < frame_seq_target + 60) {
        if (prog1_skip_log_count < 400) {
            prog1_skip_log_count++;
            l_info("[SKIP_P1] f%d mode=0x%x first=%d count=%d fb=%u window=[%d,%d)",
                   gl_dbg_frame, mode, first, count, gl_bound_framebuffer,
                   frame_seq_target, frame_seq_target + 60);
        }
        return;
    }
    glDrawArrays(mode, first, count);
}

static void glDrawElements_hook(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    gl_draw_count++;
    glDrawElements(mode, count, type, indices);
}

static void glUseProgram_hook(GLuint program) {
    gl_current_program_fix = program;
    gl_current_program = program;
    gl_useprogram_count++;
    if (gl_dbg_frame == frame_seq_target && !frame_seq_logged)
        l_info("[seq] f%d UseProgram(%d)", gl_dbg_frame, program);
    glUseProgram(program);
}

static void glClear_hook(GLbitfield mask) {
    gl_clear_count++;
    if (gl_dbg_frame == frame_seq_target && !frame_seq_logged)
        l_info("[seq] f%d glClear(0x%x)", gl_dbg_frame, mask);
    glClear(mask);
}

static int glBindFB_count = 0;
static void glBindFramebuffer_hook(GLenum target, GLuint framebuffer) {
    glBindFB_count++;
    gl_bound_framebuffer = framebuffer;
    if (target == GL_FRAMEBUFFER && (framebuffer != 0 || gl_current_program == 5 || gl_current_program == 1)) {
        l_info("[FBIND] f%d prog=%u target=0x%x fb=%u", gl_dbg_frame, gl_current_program, target, framebuffer);
    }
    glBindFramebuffer(target, framebuffer);
}

static GLenum glCheckFramebufferStatus_hook(GLenum target) {
    GLenum status = glCheckFramebufferStatus(target);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        l_info("[gl_dbg] glCheckFramebufferStatus(0x%x) = 0x%x INCOMPLETE!\n", target, status);
    else if (gl_dbg_frame <= 3)
        l_info("[gl_dbg] f%d glCheckFramebufferStatus(0x%x) = COMPLETE\n", gl_dbg_frame, target);
    return status;
}

static void glViewport_hook(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (gl_dbg_frame == frame_seq_target && !frame_seq_logged)
        l_info("[seq] f%d Viewport(%d,%d,%d,%d)", gl_dbg_frame, x, y, w, h);
    glViewport(x, y, w, h);
}

static int glFBTex_count = 0;
static void glFramebufferTexture2D_hook(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    glFBTex_count++;
    l_info("[FATTACH] f%d prog=%u fb=%u target=0x%x attach=0x%x textarget=0x%x tex=%u level=%d",
           gl_dbg_frame, gl_current_program, gl_bound_framebuffer, target, attachment, textarget, texture, level);
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

static void glGenFramebuffers_hook(GLsizei n, GLuint *framebuffers) {
    glGenFramebuffers(n, framebuffers);
    l_info("[gl_dbg] glGenFramebuffers(%d) -> fb=%d\n", n, framebuffers ? framebuffers[0] : -1);
}

static void glGenRenderbuffers_hook(GLsizei n, GLuint *renderbuffers) {
    glGenRenderbuffers(n, renderbuffers);
    l_info("[gl_dbg] glGenRenderbuffers(%d) -> rb=%d\n", n, renderbuffers ? renderbuffers[0] : -1);
}

static void glRenderbufferStorage_hook(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    l_info("[gl_dbg] glRenderbufferStorage(fmt=0x%x, %dx%d)\n", internalformat, width, height);
    glRenderbufferStorage(target, internalformat, width, height);
}

static void glFramebufferRenderbuffer_hook(GLenum target, GLenum attachment, GLenum rbTarget, GLuint renderbuffer) {
    l_info("[gl_dbg] glFramebufferRenderbuffer(attach=0x%x, rb=%d)\n", attachment, renderbuffer);
    glFramebufferRenderbuffer(target, attachment, rbTarget, renderbuffer);
}

static int max_attribs_logged = 0;
static void glBindAttribLocation_hook(GLuint program, GLuint index, const GLchar *name) {
    if (!max_attribs_logged) {
        GLint max_attribs = 0;
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attribs);
        l_info("[gl_dbg] GL_MAX_VERTEX_ATTRIBS = %d", max_attribs);
        max_attribs_logged = 1;
    }
    if (is_maze_program(program))
        l_info("[BINDATTR] prog=%d idx=%d name=\"%s\"", program, index, name ? name : "(null)");
    else
        l_info("[gl_dbg] glBindAttribLocation(prog=%d, idx=%d, \"%s\")", program, index, name ? name : "(null)");
    glBindAttribLocation(program, index, name);
}

static int glEnableVAA_log_count = 0;
static void glEnableVertexAttribArray_hook(GLuint index) {
    if (glEnableVAA_log_count < 50) {
        glEnableVAA_log_count++;
        l_info("[gl_dbg] glEnableVertexAttribArray(%d)", index);
    }
    glEnableVertexAttribArray(index);
}

static int glDisableVAA_log_count = 0;
static void glDisableVertexAttribArray_hook(GLuint index) {
    if (glDisableVAA_log_count < 50) {
        glDisableVAA_log_count++;
        l_info("[gl_dbg] glDisableVertexAttribArray(%d)", index);
    }
    glDisableVertexAttribArray(index);
}

static GLint glGetAttribLocation_hook(GLuint program, const GLchar *name) {
    GLint loc = glGetAttribLocation(program, name);
    if (is_maze_program(program))
        l_info("[ALOC] prog=%d \"%s\" = %d", program, name ? name : "(null)", loc);
    if (loc == -1)
        l_info("[gl_dbg] glGetAttribLocation(prog=%d, \"%s\") = -1", program, name ? name : "(null)");
    return loc;
}

static GLint glGetUniformLocation_hook(GLuint program, const GLchar *name) {
    GLint loc = glGetUniformLocation(program, name);
    /* Log ALL uniform queries for maze-related programs */
    if (is_maze_program(program)) {
        l_info("[ULOC] prog=%d \"%s\" = %d (0x%x)", program, name ? name : "(null)", loc, (unsigned)loc);
    }
    if (loc == -1 && name) {
        /* vitaGL/CG may strip array index from uniform names.
         * If "foo[0]" fails, retry with just "foo". */
        const char *bracket = strchr(name, '[');
        if (bracket) {
            size_t base_len = bracket - name;
            char base_name[128];
            if (base_len < sizeof(base_name)) {
                memcpy(base_name, name, base_len);
                base_name[base_len] = '\0';
                loc = glGetUniformLocation(program, base_name);
                if (is_maze_program(program))
                    l_info("[ULOC] prog=%d retry \"%s\" = %d (0x%x)", program, base_name, loc, (unsigned)loc);
                else
                    l_info("[gl_dbg] glGetUniformLocation(prog=%d, \"%s\") = -1, retry \"%s\" = %d",
                           program, name, base_name, loc);
            }
        } else if (program != 5) {
            l_info("[gl_dbg] glGetUniformLocation(prog=%d, \"%s\") = -1", program, name);
        }
    }
    return loc;
}

/* Track last glVertexAttribPointer calls for prog 5 diagnosis */
static int vap_log_count = 0;

static void glVertexAttribPointer_hook(GLuint index, GLint size, GLenum type,
                                        GLboolean normalized, GLsizei stride,
                                        const void *pointer) {
    if (index < 16) {
        vap_state[index].size = size;
        vap_state[index].type = type;
        vap_state[index].stride = stride;
        vap_state[index].pointer = pointer;
        vap_state[index].normalized = normalized;
    }
    /* Log first 20 calls + all calls when prog 5 is active */
    if (vap_log_count < 20 || gl_current_program == 5) {
        vap_log_count++;
        l_info("[VAP] prog=%d idx=%d size=%d type=0x%x norm=%d stride=%d ptr=%p",
               gl_current_program, index, size, type, normalized, stride, pointer);
        /* Try to read first 16 bytes at the pointer to check accessibility */
        if (pointer) {
            const unsigned char *p = (const unsigned char *)pointer;
            l_info("[VAP]   data: %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                   p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
            /* Decode first vertex as floats */
            const float *fp = (const float *)pointer;
            l_info("[VAP]   floats: %f %f %f %f", fp[0], fp[1], fp[2], fp[3]);
        }
    }
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

static void glUniformMatrix4fv_hook(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (is_maze_program(gl_current_program) && !maze_mat4_dumped[gl_current_program] && value) {
        maze_mat4_dumped[gl_current_program] = 1;
        l_info("[MAT4] prog=%d loc=%d count=%d transpose=%d", gl_current_program, location, count, transpose);
        l_info("[MAT4]  %f %f %f %f", value[0], value[1], value[2], value[3]);
        l_info("[MAT4]  %f %f %f %f", value[4], value[5], value[6], value[7]);
        l_info("[MAT4]  %f %f %f %f", value[8], value[9], value[10], value[11]);
        l_info("[MAT4]  %f %f %f %f", value[12], value[13], value[14], value[15]);
    }
    glUniformMatrix4fv(location, count, transpose, value);
}

static void glUniform1i_hook(GLint location, GLint v0) {
    if (is_maze_program(gl_current_program)) {
        l_info("[U1I] prog=%d loc=%d val=%d", gl_current_program, location, v0);
    }
    glUniform1i(location, v0);
}

static void glActiveTexture_hook(GLenum texture) {
    gl_active_texture_unit = texture;
    if (is_maze_program(gl_current_program)) {
        l_info("[ATEX] prog=%d unit=%d", gl_current_program, texture - GL_TEXTURE0);
    }
    glActiveTexture(texture);
}

static void glBindTexture_hook(GLenum target, GLuint texture) {
    if (is_maze_program(gl_current_program) || texture == 51 || texture == 52) {
        l_info("[BTEX] prog=%d unit=%d target=0x%x tex=%u",
               gl_current_program, gl_active_texture_unit - GL_TEXTURE0, target, texture);
    }
    glBindTexture(target, texture);
}

static void glCopyTexImage2D_hook(GLenum target, GLint level, GLenum internalformat,
                                  GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    l_info("[CTEX] f%d prog=%u fb=%u glCopyTexImage2D(target=0x%x, level=%d, ifmt=0x%x, x=%d, y=%d, w=%d, h=%d, border=%d)",
           gl_dbg_frame, gl_current_program, gl_bound_framebuffer, target, level, internalformat, x, y, width, height, border);
    glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

static void glCopyTexSubImage2D_hook(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                     GLint x, GLint y, GLsizei width, GLsizei height) {
    l_info("[CTEX] f%d prog=%u fb=%u glCopyTexSubImage2D(target=0x%x, level=%d, xoff=%d, yoff=%d, x=%d, y=%d, w=%d, h=%d)",
           gl_dbg_frame, gl_current_program, gl_bound_framebuffer, target, level, xoffset, yoffset, x, y, width, height);
    glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

#endif

/*
 * Vita3K workaround: force all color channels writable.
 * Vita3K's Vulkan renderer doesn't implement GXM output masks
 * ("Mask not implemented in the vulkan renderer!") and hangs
 * when a shader with a partial color mask is used.
 */
static void glColorMask_vita3k(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    (void)r; (void)g; (void)b; (void)a;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* Android AT_HWCAP bits for ARM */
#define COMPAT_HWCAP_NEON   (1 << 12)
#define COMPAT_HWCAP_VFPv3  (1 << 13)
#define AT_HWCAP  16
#define AT_HWCAP2 26
static unsigned long getauxval_soloader(unsigned long type) {
    if (type == AT_HWCAP) {
        /* Report VFPv3 only — do NOT report NEON.
         * FMOD's NEON-optimized mixer uses instructions that Vita3K's
         * ARM interpreter doesn't implement, causing a coprocessor
         * exception crash.  Without NEON, FMOD falls back to scalar. */
        return COMPAT_HWCAP_VFPv3;
    }
    return 0;
}

so_default_dynlib default_dynlib[] = {
        // Common C/C++ internals
        { "_ZNSt8bad_castD1Ev", (uintptr_t)&_ZNSt8bad_castD1Ev },
        { "_ZNSt9exceptionD2Ev", (uintptr_t)&_ZNSt9exceptionD2Ev },
        { "_ZSt17__throw_bad_allocv", (uintptr_t)&_ZSt17__throw_bad_allocv },
        { "_ZSt9terminatev", (uintptr_t)&_ZSt9terminatev },
        { "_ZTISt8bad_cast", (uintptr_t)&_ZTISt8bad_cast },
        { "_ZTISt9exception", (uintptr_t)&_ZTISt9exception },
        { "_ZTVN10__cxxabiv117__class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE },
        { "_ZTVN10__cxxabiv120__si_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv120__si_class_type_infoE },
        { "_ZTVN10__cxxabiv121__vmi_class_type_infoE", (uintptr_t)&_ZTVN10__cxxabiv121__vmi_class_type_infoE },
        { "_ZdaPv", (uintptr_t)&_ZdaPv },
        { "_ZdlPv", (uintptr_t)&_ZdlPv },
        { "_Znaj", (uintptr_t)&_Znaj },
        { "_Znwj", (uintptr_t)&_Znwj },
        { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
        { "__aeabi_d2lz", (uintptr_t)&__aeabi_d2lz },
        { "__aeabi_d2ulz", (uintptr_t)&__aeabi_d2ulz },
        { "__aeabi_dadd", (uintptr_t)&__aeabi_dadd },
        { "__aeabi_dcmpgt", (uintptr_t)&__aeabi_dcmpgt },
        { "__aeabi_dcmplt", (uintptr_t)&__aeabi_dcmplt },
        { "__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv },
        { "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
        { "__aeabi_f2lz", (uintptr_t)&__aeabi_f2lz },
        { "__aeabi_f2ulz", (uintptr_t)&__aeabi_f2ulz },
        { "__aeabi_i2d", (uintptr_t)&__aeabi_i2d },
        { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
        { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
        { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
        { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
        { "__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod },
        { "__aeabi_memclr", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr4", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memclr8", (uintptr_t)&__aeabi_memclr },
        { "__aeabi_memcpy", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy },
        { "__aeabi_memmove", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove4", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memmove8", (uintptr_t)&__aeabi_memmove },
        { "__aeabi_memset", (uintptr_t)&__aeabi_memset },
        { "__aeabi_memset4",  (uintptr_t)&__aeabi_memset4 },
        { "__aeabi_memset8", (uintptr_t)&__aeabi_memset8 },
        { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
        { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
        { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
        { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
        { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
        { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },
        { "__aeabi_unwind_cpp_pr0", (uintptr_t)&__aeabi_unwind_cpp_pr0 },
        { "__aeabi_unwind_cpp_pr1", (uintptr_t)&__aeabi_unwind_cpp_pr1 },
        { "__atomic_cmpxchg", (uintptr_t)&__atomic_cmpxchg },
        { "__atomic_dec", (uintptr_t)&__atomic_dec },
        { "__atomic_inc", (uintptr_t)&__atomic_inc },
        { "__atomic_swap", (uintptr_t)&__atomic_swap },
        { "__cxa_allocate_exception", (uintptr_t)&__cxa_allocate_exception },
        { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
        { "__cxa_begin_catch", (uintptr_t)&__cxa_begin_catch },
        { "__cxa_begin_cleanup", (uintptr_t)&ret0 },
        { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
        { "__cxa_end_catch", (uintptr_t)&__cxa_end_catch },
        { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
        { "__cxa_free_exception", (uintptr_t)&__cxa_free_exception },
        { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
        { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
        { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
        { "__cxa_rethrow", (uintptr_t)&__cxa_rethrow },
        { "__cxa_throw", (uintptr_t)&__cxa_throw },
        { "__cxa_type_match", (uintptr_t)&ret0 },
        { "__gnu_Unwind_Find_exidx", (uintptr_t)&ret0 },
        { "__gnu_ldivmod_helper", (uintptr_t)&__gnu_ldivmod_helper },
        { "__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame },
        { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
        { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
        { "__gxx_personality_v0", (uintptr_t)&__gxx_personality_v0 },
        { "__isinf", (uintptr_t)&ret0 },
        { "__page_size", (uintptr_t)&__page_size },
        { "__sF", (uintptr_t)&__sF_fake },
        { "__srget", (uintptr_t)&__srget },
        { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_soloader },
        { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
        { "__swbuf", (uintptr_t)&__swbuf },
        { "__system_property_get", (uintptr_t)&__system_property_get_soloader },
        { "__assert2", (uintptr_t)&ret0 }, // TODO: stub/impl
        { "dl_unwind_find_exidx", (uintptr_t)&ret0 }, // TODO: stub/impl


        // ctype
        { "_ctype_", (uintptr_t)&BIONIC_ctype_ },
        { "_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_ },
        { "_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_ },
        { "isalnum", (uintptr_t)&isalnum },
        { "isalpha", (uintptr_t)&isalpha },
        { "isblank", (uintptr_t)&isblank },
        { "iscntrl", (uintptr_t)&iscntrl },
        { "isgraph", (uintptr_t)&isgraph },
        { "islower", (uintptr_t)&islower },
        { "isprint", (uintptr_t)&isprint },
        { "ispunct", (uintptr_t)&ispunct },
        { "isspace", (uintptr_t)&isspace },
        { "isupper", (uintptr_t)&isupper },
        { "isascii", (uintptr_t)&isascii },
        { "isdigit", (uintptr_t)&isdigit },
        { "isxdigit", (uintptr_t)&isxdigit },
        { "tolower", (uintptr_t)&tolower },
        { "toupper", (uintptr_t)&toupper },


        // Android SDK standard logging
        { "__android_log_assert", (uintptr_t)&__android_log_assert },
        { "__android_log_print", (uintptr_t)&__android_log_print },
        { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
        { "__android_log_write", (uintptr_t)&__android_log_write },


        // AAssetManager
        { "AAsset_close", (uintptr_t)&AAsset_close },
        { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
        { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength },
        { "AAsset_read", (uintptr_t)&AAsset_read },
        { "AAsset_seek", (uintptr_t)&AAsset_seek },
        { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor },
        { "AAssetDir_close", (uintptr_t)&AAssetDir_close },
        { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
        { "AAssetManager_fromJava", (uintptr_t)&ret1 },
        { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
        { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },


        // Math (soft-float ABI wrappers — Android .so uses softfp, Vita uses hardfp)
        { "acos", (uintptr_t)&acos_sfp },
        { "acosf", (uintptr_t)&acosf_sfp },
        { "asin", (uintptr_t)&asin_sfp },
        { "asinf", (uintptr_t)&asinf_sfp },
        { "atan", (uintptr_t)&atan_sfp },
        { "atan2", (uintptr_t)&atan2_sfp },
        { "atan2f", (uintptr_t)&atan2f_sfp },
        { "atanf", (uintptr_t)&atanf_sfp },
        { "ceil", (uintptr_t)&ceil_sfp },
        { "ceilf", (uintptr_t)&ceilf_sfp },
        { "cos", (uintptr_t)&cos_sfp },
        { "cosf", (uintptr_t)&cosf_sfp },
        { "exp", (uintptr_t)&exp_sfp },
        { "exp2", (uintptr_t)&exp2_sfp },
        { "exp2f", (uintptr_t)&exp2f_sfp },
        { "expf", (uintptr_t)&expf_sfp },
        { "floor", (uintptr_t)&floor_sfp },
        { "floorf", (uintptr_t)&floorf_sfp },
        { "fmax", (uintptr_t)&fmax_sfp },
        { "fmaxf", (uintptr_t)&fmaxf_sfp },
        { "fmin", (uintptr_t)&fmin_sfp },
        { "fminf", (uintptr_t)&fminf_sfp },
        { "frexpf", (uintptr_t)&frexpf_sfp },
        { "fmod", (uintptr_t)&fmod_sfp },
        { "fmodf", (uintptr_t)&fmodf_sfp },
        { "frexp", (uintptr_t)&frexp_sfp },
        { "ldexp", (uintptr_t)&ldexp_sfp },
        { "ldexpf", (uintptr_t)&ldexpf_sfp },
        { "log", (uintptr_t)&log_sfp },
        { "log10", (uintptr_t)&log10_sfp },
        { "log10f", (uintptr_t)&log10f_sfp },
        { "logf", (uintptr_t)&logf_sfp },
        { "lrint", (uintptr_t)&lrint_sfp },
        { "lrintf", (uintptr_t)&lrintf_sfp },
        { "lround", (uintptr_t)&lround_sfp },
        { "lroundf", (uintptr_t)&lroundf_sfp },
        { "modf", (uintptr_t)&modf_sfp },
        { "pow", (uintptr_t)&pow_sfp },
        { "powf", (uintptr_t)&powf_sfp },
        { "rint", (uintptr_t)&rint_sfp },
        { "rintf", (uintptr_t)&rintf_sfp },
        { "round", (uintptr_t)&round_sfp },
        { "roundf", (uintptr_t)&roundf_sfp },
        { "scalbn", (uintptr_t)&scalbn_sfp },
        { "scalbnf", (uintptr_t)&scalbnf_sfp },
        { "sin", (uintptr_t)&sin_sfp },
        { "sincos", (uintptr_t)&sincos_sfp },
        { "sincosf", (uintptr_t)&sincosf_sfp },
        { "sinf", (uintptr_t)&sinf_sfp },
        { "sinh", (uintptr_t)&sinh_sfp },
        { "sinhf", (uintptr_t)&sinhf_sfp },
        { "sqrt", (uintptr_t)&sqrt_sfp },
        { "sqrtf", (uintptr_t)&sqrtf_sfp },
        { "tan", (uintptr_t)&tan_sfp },
        { "tanf", (uintptr_t)&tanf_sfp },
        { "tanh", (uintptr_t)&tanh_sfp },
        { "trunc", (uintptr_t)&trunc_sfp },
        { "truncf", (uintptr_t)&truncf_sfp },


        // Sockets
        { "accept", (uintptr_t)&accept },
        { "bind", (uintptr_t)&bind },
        { "connect", (uintptr_t)&connect },
        { "freeaddrinfo", (uintptr_t)&freeaddrinfo },
        { "gai_strerror", (uintptr_t)&ret0 },
        { "getaddrinfo", (uintptr_t)&getaddrinfo },
        { "gethostbyaddr", (uintptr_t)&gethostbyaddr },
        { "gethostbyname", (uintptr_t)&gethostbyname },
        { "gethostname", (uintptr_t)&gethostname },
        { "getpeername", (uintptr_t)&getpeername },
        { "getservbyname", (uintptr_t)&getservbyname },
        { "getsockname", (uintptr_t)&getsockname },
        { "getsockopt", (uintptr_t)&getsockopt },
        { "inet_aton", (uintptr_t)&inet_aton },
        { "inet_pton", (uintptr_t)&inet_pton },
        { "inet_ntoa", (uintptr_t)&inet_ntoa },
        { "inet_ntop", (uintptr_t)&inet_ntop },
        { "listen", (uintptr_t)&listen },
        { "poll", (uintptr_t)&poll },
        { "recv", (uintptr_t)&recv },
        { "recvfrom", (uintptr_t)&recvfrom },
        { "recvmsg", (uintptr_t)&recvmsg },
        { "select", (uintptr_t)&select },
        { "send", (uintptr_t)&send },
        { "sendmsg", (uintptr_t)&sendmsg },
        { "sendto", (uintptr_t)&sendto },
        { "setsockopt", (uintptr_t)&setsockopt },
        { "shutdown", (uintptr_t)&shutdown },
        { "socket", (uintptr_t)&socket },


        // Memory
        { "calloc", (uintptr_t)&calloc },
        { "free", (uintptr_t)&free },
        { "malloc", (uintptr_t)&malloc },
        { "memalign", (uintptr_t)&memalign },
        { "memcmp", (uintptr_t)&memcmp },
        { "memcpy", (uintptr_t)&sceClibMemcpy },
        { "memmem", (uintptr_t)&memmem },
        { "memmove", (uintptr_t)&memmove },
        { "memset", (uintptr_t)&memset },
        { "mmap", (uintptr_t)&mmap },
        { "__mmap2", (uintptr_t)&mmap },
        { "munmap", (uintptr_t)&munmap },
        { "realloc", (uintptr_t)&realloc },
        { "valloc", (uintptr_t)&valloc },


        // IO
        { "close", (uintptr_t)&close_soloader },
        { "closedir", (uintptr_t)&closedir_soloader },
        { "execv", (uintptr_t)&ret0 },
        { "fclose", (uintptr_t)&fclose_soloader },
        { "fcntl", (uintptr_t)&fcntl_soloader },
        { "fopen", (uintptr_t)&fopen_soloader },
        { "fstat", (uintptr_t)&fstat_soloader },
        { "fsync", (uintptr_t)&fsync_soloader },
        { "ioctl", (uintptr_t)&ioctl_soloader },
        { "__open_2", (uintptr_t)&open_soloader },
        { "open", (uintptr_t)&open_soloader },
        { "opendir", (uintptr_t)&opendir_soloader },
        { "readdir", (uintptr_t)&readdir_soloader },
        { "readdir_r", (uintptr_t)&readdir_r_soloader },
        { "stat", (uintptr_t)&stat_soloader },
        { "utime", (uintptr_t)&utime },

        #ifdef USE_SCELIBC_IO
            { "fdopen", (uintptr_t)&sceLibcBridge_fdopen },
            { "feof", (uintptr_t)&sceLibcBridge_feof },
            { "ferror", (uintptr_t)&sceLibcBridge_ferror },
            { "fflush", (uintptr_t)&sceLibcBridge_fflush },
            { "fgetc", (uintptr_t)&sceLibcBridge_fgetc },
            { "fgetpos", (uintptr_t)&sceLibcBridge_fgetpos },
            { "fgets", (uintptr_t)&sceLibcBridge_fgets },
            { "fileno", (uintptr_t)&sceLibcBridge_fileno },
            { "fputc", (uintptr_t)&sceLibcBridge_fputc },
            { "fputs", (uintptr_t)&sceLibcBridge_fputs },
            { "fread", (uintptr_t)&fread_soloader },
            { "freopen", (uintptr_t)&sceLibcBridge_freopen },
            { "fseek", (uintptr_t)&fseek_soloader },
            { "fsetpos", (uintptr_t)&sceLibcBridge_fsetpos },
            { "ftell", (uintptr_t)&ftell_soloader },
            { "fwide", (uintptr_t)&sceLibcBridge_fwide },
            { "fwrite", (uintptr_t)&sceLibcBridge_fwrite },
            { "getc", (uintptr_t)&sceLibcBridge_getc },
            { "getwc", (uintptr_t)&sceLibcBridge_getwc },
            { "putc", (uintptr_t)&sceLibcBridge_putc },
            { "putchar", (uintptr_t)&sceLibcBridge_putchar },
            { "puts", (uintptr_t)&sceLibcBridge_puts },
            { "putwc", (uintptr_t)&sceLibcBridge_putwc },
            { "setvbuf", (uintptr_t)&sceLibcBridge_setvbuf },
            { "ungetc", (uintptr_t)&sceLibcBridge_ungetc },
            { "ungetwc", (uintptr_t)&sceLibcBridge_ungetwc },
        #else
            { "fdopen", (uintptr_t)&fdopen },
            { "feof", (uintptr_t)&feof },
            { "ferror", (uintptr_t)&ferror },
            { "fflush", (uintptr_t)&fflush },
            { "fgetc", (uintptr_t)&fgetc },
            { "fgetpos", (uintptr_t)&fgetpos },
            { "fgets", (uintptr_t)&fgets },
            { "fileno", (uintptr_t)&fileno },
            { "fputc", (uintptr_t)&fputc },
            { "fputs", (uintptr_t)&fputs },
            { "fread", (uintptr_t)&fread_soloader },
            { "freopen", (uintptr_t)&freopen },
            { "fseek", (uintptr_t)&fseek_soloader },
            { "fsetpos", (uintptr_t)&fsetpos },
            { "ftell", (uintptr_t)&ftell_soloader },
            { "fwide", (uintptr_t)&fwide },
            { "fwrite", (uintptr_t)&fwrite },
            { "getc", (uintptr_t)&getc },
            { "getwc", (uintptr_t)&getwc },
            { "putc", (uintptr_t)&putc },
            { "putchar", (uintptr_t)&putchar },
            { "puts", (uintptr_t)&puts },
            { "putwc", (uintptr_t)&putwc },
            { "setvbuf", (uintptr_t)&setvbuf },
            { "ungetc", (uintptr_t)&ungetc },
            { "ungetwc", (uintptr_t)&ungetwc },
        #endif

        { "clearerr", (uintptr_t)&clearerr },
        { "access", (uintptr_t)&access },
        { "basename", (uintptr_t)&basename },
        { "chdir", (uintptr_t)&chdir },
        { "chmod", (uintptr_t)&chmod },
        { "dup", (uintptr_t)&dup },
        { "fseeko", (uintptr_t)&fseek_soloader },
        { "ftello", (uintptr_t)&ftell_soloader },
        { "ftruncate", (uintptr_t)&ftruncate },
        { "getcwd", (uintptr_t)&getcwd },
        { "lseek", (uintptr_t)&lseek },
        { "lseek64", (uintptr_t)&ret0 }, // TODO: implement or stub with warning
        { "lstat", (uintptr_t)&lstat },
        { "mkdir", (uintptr_t)&mkdir },
        { "pipe", (uintptr_t)&pipe },
        { "read", (uintptr_t)&read },
        { "realpath", (uintptr_t)&realpath },
        { "remove", (uintptr_t)&remove },
        { "rename", (uintptr_t)&rename },
        { "rewind", (uintptr_t)&rewind },
        { "rmdir", (uintptr_t)&rmdir },
        { "truncate", (uintptr_t)&truncate },
        { "unlink", (uintptr_t)&unlink },
        { "write", (uintptr_t)&write },


        // *printf, *scanf
        { "snprintf", (uintptr_t)&snprintf },
        { "sprintf", (uintptr_t)&sprintf },
        { "vasprintf", (uintptr_t)&vasprintf },
        { "vprintf", (uintptr_t)&vprintf },
        { "vsnprintf", (uintptr_t)&vsnprintf },
        { "vsprintf", (uintptr_t)&vsprintf },
        { "vsscanf", (uintptr_t)&vsscanf },
        { "vswprintf", (uintptr_t)&vswprintf },
        { "printf", (uintptr_t)&sceClibPrintf },
        { "swprintf", (uintptr_t)&swprintf },

        #ifdef USE_SCELIBC_IO
            { "fprintf", (uintptr_t)&sceLibcBridge_fprintf },
            { "fscanf", (uintptr_t)&sceLibcBridge_fscanf },
            { "sscanf", (uintptr_t)&sceLibcBridge_sscanf },
            { "vfprintf", (uintptr_t)&sceLibcBridge_vfprintf },
        #else
            { "fprintf", (uintptr_t)&fprintf },
            { "fscanf", (uintptr_t)&fscanf },
            { "sscanf", (uintptr_t)&sscanf },
            { "vfprintf", (uintptr_t)&vfprintf },
        #endif


        // EGL
        { "eglBindAPI", (uintptr_t)&eglBindAPI },
        { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
        { "eglCreateContext", (uintptr_t)&eglCreateContext },
        { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
        { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
        { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
        { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
        { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
        { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
        { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
        { "eglGetError", (uintptr_t)&eglGetError },
        { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
        { "eglInitialize", (uintptr_t)&eglInitialize },
        { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
        { "eglQueryContext", (uintptr_t)&eglQueryContext },
        { "eglQueryString", (uintptr_t)&eglQueryString },
        { "eglQuerySurface", (uintptr_t)&eglQuerySurface },
        { "eglSwapBuffers", (uintptr_t)&eglSwapBuffers },
        { "eglTerminate", (uintptr_t)&eglTerminate },


        // OpenGL
#ifdef DEBUG_OPENGL
        { "glActiveTexture", (uintptr_t)&glActiveTexture_hook },
#else
        { "glActiveTexture", (uintptr_t)&glActiveTexture },
#endif
        { "glAlphaFunc", (uintptr_t)&glAlphaFunc_sfp },
        { "glAlphaFuncx", (uintptr_t)&glAlphaFuncx },
        { "glAttachShader", (uintptr_t)&glAttachShader_soloader },
#ifdef DEBUG_OPENGL
        { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation_hook },
#else
        { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
#endif
        { "glBindBuffer", (uintptr_t)&glBindBuffer },
#ifdef DEBUG_OPENGL
        { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer_hook },
        { "glBindFramebufferOES", (uintptr_t)&glBindFramebuffer_hook },
#else
        { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
        { "glBindFramebufferOES", (uintptr_t)&glBindFramebuffer },
#endif
        { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
        { "glBindRenderbufferOES", (uintptr_t)&glBindRenderbuffer },
#ifdef DEBUG_OPENGL
        { "glBindTexture", (uintptr_t)&glBindTexture_hook },
#else
        { "glBindTexture", (uintptr_t)&glBindTexture },
#endif
        { "glBlendColor", (uintptr_t)&ret0 },
        { "glBlendEquation", (uintptr_t)&glBlendEquation },
        { "glBlendEquationOES", (uintptr_t)&glBlendEquation },
        { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendEquationSeparateOES", (uintptr_t)&glBlendEquationSeparate },
        { "glBlendFunc", (uintptr_t)&glBlendFunc },
        { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
        { "glBlendFuncSeparateOES", (uintptr_t)&glBlendFuncSeparate },
        { "glBufferData", (uintptr_t)&glBufferData },
        { "glBufferSubData", (uintptr_t)&glBufferSubData },
#ifdef DEBUG_OPENGL
        { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus_hook },
        { "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatus_hook },
#else
        { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
        { "glCheckFramebufferStatusOES", (uintptr_t)&glCheckFramebufferStatus },
#endif
#ifdef DEBUG_OPENGL
        { "glClear", (uintptr_t)&glClear_hook },
#else
        { "glClear", (uintptr_t)&glClear },
#endif
        { "glClearColor", (uintptr_t)&glClearColor_sfp },
        { "glClearColorx", (uintptr_t)&glClearColorx },
        { "glClearDepthf", (uintptr_t)&glClearDepthf_sfp },
        { "glClearDepthx", (uintptr_t)&glClearDepthx },
        { "glClearStencil", (uintptr_t)&glClearStencil },
        { "glClientActiveTexture", (uintptr_t)&glClientActiveTexture },
        { "glClipPlanef", (uintptr_t)&glClipPlanef },
        { "glClipPlanex", (uintptr_t)&glClipPlanex },
        { "glColor4f", (uintptr_t)&glColor4f_sfp },
        { "glColor4ub", (uintptr_t)&glColor4ub },
        { "glColor4x", (uintptr_t)&glColor4x },
        { "glColorMask", (uintptr_t)&glColorMask_vita3k },
        { "glColorPointer", (uintptr_t)&glColorPointer },
        { "glCompileShader", (uintptr_t)&glCompileShader_soloader },
        { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
        { "glCompressedTexSubImage2D", (uintptr_t)&ret0 },
#ifdef DEBUG_OPENGL
        { "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D_hook },
        { "glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D_hook },
#else
        { "glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D },
        { "glCopyTexSubImage2D", (uintptr_t)&glCopyTexSubImage2D },
#endif
        { "glCreateProgram", (uintptr_t)&glCreateProgram_soloader },
        { "glCreateShader", (uintptr_t)&glCreateShader_soloader },
        { "glCullFace", (uintptr_t)&glCullFace },
        { "glCurrentPaletteMatrixOES", (uintptr_t)&ret0 },
        { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
        { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteFramebuffersOES", (uintptr_t)&glDeleteFramebuffers },
        { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
        { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteRenderbuffersOES", (uintptr_t)&glDeleteRenderbuffers },
        { "glDeleteShader", (uintptr_t)&glDeleteShader },
        { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
        { "glDepthFunc", (uintptr_t)&glDepthFunc },
        { "glDepthMask", (uintptr_t)&glDepthMask },
        { "glDepthRangef", (uintptr_t)&glDepthRangef_sfp },
        { "glDepthRangex", (uintptr_t)&glDepthRangex },
        { "glDetachShader", (uintptr_t)&ret0 },
        { "glDisable", (uintptr_t)&glDisable },
        { "glDisableClientState", (uintptr_t)&glDisableClientState },
#ifdef DEBUG_OPENGL
        { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray_hook },
#else
        { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
#endif
#ifdef DEBUG_OPENGL
        { "glDrawArrays", (uintptr_t)&glDrawArrays_hook },
#else
        { "glDrawArrays", (uintptr_t)&glDrawArrays_fix },
#endif
#ifdef DEBUG_OPENGL
        { "glDrawElements", (uintptr_t)&glDrawElements_hook },
#else
        { "glDrawElements", (uintptr_t)&glDrawElements },
#endif
        { "glDrawTexfOES", (uintptr_t)&ret0 },
        { "glDrawTexfvOES", (uintptr_t)&ret0 },
        { "glDrawTexiOES", (uintptr_t)&ret0 },
        { "glDrawTexivOES", (uintptr_t)&ret0 },
        { "glDrawTexsOES", (uintptr_t)&ret0 },
        { "glDrawTexsvOES", (uintptr_t)&ret0 },
        { "glDrawTexxOES", (uintptr_t)&ret0 },
        { "glDrawTexxvOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetRenderbufferStorageOES", (uintptr_t)&ret0 },
        { "glEGLImageTargetTexture2DOES", (uintptr_t)&ret0 },
        { "glEnable", (uintptr_t)&glEnable },
        { "glEnableClientState", (uintptr_t)&glEnableClientState },
#ifdef DEBUG_OPENGL
        { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray_hook },
#else
        { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
#endif
        { "glFinish", (uintptr_t)&glFinish },
        { "glFlush", (uintptr_t)&glFlush },
        { "glFogf", (uintptr_t)&glFogf_sfp },
        { "glFogfv", (uintptr_t)&glFogfv },
        { "glFogx", (uintptr_t)&glFogx },
        { "glFogxv", (uintptr_t)&glFogxv },
#ifdef DEBUG_OPENGL
        { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer_hook },
        { "glFramebufferRenderbufferOES", (uintptr_t)&glFramebufferRenderbuffer_hook },
#else
        { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
        { "glFramebufferRenderbufferOES", (uintptr_t)&glFramebufferRenderbuffer },
#endif
#ifdef DEBUG_OPENGL
        { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D_hook },
        { "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2D_hook },
#else
        { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
        { "glFramebufferTexture2DOES", (uintptr_t)&glFramebufferTexture2D },
#endif
        { "glFrontFace", (uintptr_t)&glFrontFace },
        { "glFrustumf", (uintptr_t)&glFrustumf_sfp },
        { "glFrustumx", (uintptr_t)&glFrustumx },
        { "glGenBuffers", (uintptr_t)&glGenBuffers },
        { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
        { "glGenerateMipmapOES", (uintptr_t)&glGenerateMipmap },
#ifdef DEBUG_OPENGL
        { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers_hook },
        { "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers_hook },
        { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers_hook },
        { "glGenRenderbuffersOES", (uintptr_t)&glGenRenderbuffers_hook },
#else
        { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
        { "glGenFramebuffersOES", (uintptr_t)&glGenFramebuffers },
        { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
        { "glGenRenderbuffersOES", (uintptr_t)&glGenRenderbuffers },
#endif
        { "glGenTextures", (uintptr_t)&glGenTextures },
        { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib },
        { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
#ifdef DEBUG_OPENGL
        { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation_hook },
#else
        { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
#endif
        { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
        { "glGetBufferParameteriv", (uintptr_t)&glGetBufferParameteriv },
        { "glGetBufferPointervOES", (uintptr_t)&ret0 },
        { "glGetClipPlanef", (uintptr_t)&ret0 },
        { "glGetClipPlanex", (uintptr_t)&ret0 },
        { "glGetError", (uintptr_t)&glGetError },
        { "glGetFixedv", (uintptr_t)&ret0 },
        { "glGetFloatv", (uintptr_t)&glGetFloatv },
        { "glGetFramebufferAttachmentParameterivOES", (uintptr_t)&glGetFramebufferAttachmentParameteriv },
        { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
        { "glGetLightfv", (uintptr_t)&ret0 },
        { "glGetLightxv", (uintptr_t)&ret0 },
        { "glGetMaterialfv", (uintptr_t)&ret0 },
        { "glGetMaterialxv", (uintptr_t)&ret0 },
        { "glGetPointerv", (uintptr_t)&ret0 },
        { "glGetRenderbufferParameterivOES", (uintptr_t)&ret0 },
        { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
        { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
        { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
        { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
        { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
        { "glGetString", (uintptr_t)&glGetString },
        { "glGetTexEnvfv", (uintptr_t)&ret0 },
        { "glGetTexEnviv", (uintptr_t)&glGetTexEnviv },
        { "glGetTexEnvxv", (uintptr_t)&ret0 },
        { "glGetTexGenfvOES", (uintptr_t)&ret0 },
        { "glGetTexGenivOES", (uintptr_t)&ret0 },
        { "glGetTexGenxvOES", (uintptr_t)&ret0 },
        { "glGetTexParameterfv", (uintptr_t)&ret0 },
        { "glGetTexParameteriv", (uintptr_t)&ret0 },
        { "glGetTexParameterxv", (uintptr_t)&ret0 },
#ifdef DEBUG_OPENGL
        { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation_hook },
#else
        { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
#endif
        { "glHint", (uintptr_t)&glHint },
        { "glIsBuffer", (uintptr_t)&ret0 },
        { "glIsRenderbuffer", (uintptr_t)&glIsRenderbuffer },
        { "glIsEnabled", (uintptr_t)&glIsEnabled },
        { "glIsFramebufferOES", (uintptr_t)&glIsFramebuffer },
        { "glIsRenderbufferOES", (uintptr_t)&glIsRenderbuffer },
        { "glIsTexture", (uintptr_t)&glIsTexture },
        { "glLightf", (uintptr_t)&ret0 },
        { "glLightfv", (uintptr_t)&glLightfv },
        { "glLightModelf", (uintptr_t)&ret0 },
        { "glLightModelfv", (uintptr_t)&glLightModelfv },
        { "glLightModelx", (uintptr_t)&ret0 },
        { "glLightModelxv", (uintptr_t)&glLightModelxv },
        { "glLightx", (uintptr_t)&ret0 },
        { "glLightxv", (uintptr_t)&glLightxv },
        { "glLineWidth", (uintptr_t)&glLineWidth_sfp },
        { "glLineWidthx", (uintptr_t)&glLineWidthx },
        { "glLinkProgram", (uintptr_t)&glLinkProgram_soloader },
        { "glLoadIdentity", (uintptr_t)&glLoadIdentity },
        { "glLoadMatrixf", (uintptr_t)&glLoadMatrixf },
        { "glLoadMatrixx", (uintptr_t)&glLoadMatrixx },
        { "glLoadPaletteFromModelViewMatrixOES", (uintptr_t)&ret0 },
        { "glLogicOp", (uintptr_t)&ret0 },
        { "glMapBuffer", (uintptr_t)&glMapBuffer },
        { "glMapBufferOES", (uintptr_t)&glMapBuffer },
        { "glMaterialf", (uintptr_t)&glMaterialf },
        { "glMaterialfv", (uintptr_t)&glMaterialfv },
        { "glMaterialx", (uintptr_t)&glMaterialx },
        { "glMaterialxv", (uintptr_t)&glMaterialxv },
        { "glMatrixIndexPointerOES", (uintptr_t)&ret0 },
        { "glMatrixMode", (uintptr_t)&glMatrixMode },
        { "glMultiTexCoord4f", (uintptr_t)&ret0 },
        { "glMultiTexCoord4x", (uintptr_t)&ret0},
        { "glMultMatrixf", (uintptr_t)&glMultMatrixf },
        { "glMultMatrixx", (uintptr_t)&glMultMatrixx },
        { "glNormal3f", (uintptr_t)&glNormal3f },
        { "glNormal3x", (uintptr_t)&glNormal3x },
        { "glNormalPointer", (uintptr_t)&glNormalPointer },
        { "glOrthof", (uintptr_t)&glOrthof_sfp },
        { "glOrthox", (uintptr_t)&glOrthox },
        { "glPixelStorei", (uintptr_t)&glPixelStorei },
        { "glPointParameterf", (uintptr_t)&ret0 },
        { "glPointParameterfv", (uintptr_t)&ret0 },
        { "glPointParameterx", (uintptr_t)&ret0 },
        { "glPointParameterxv", (uintptr_t)&ret0 },
        { "glPointSize", (uintptr_t)&glPointSize_sfp },
        { "glPointSizePointerOES", (uintptr_t)&ret0 },
        { "glPointSizex", (uintptr_t)&glPointSizex },
        { "glPolygonOffset", (uintptr_t)&glPolygonOffset_sfp },
        { "glPolygonOffsetx", (uintptr_t)&glPolygonOffsetx },
        { "glPopMatrix", (uintptr_t)&glPopMatrix },
        { "glPushMatrix", (uintptr_t)&glPushMatrix },
        { "glQueryMatrixxOES", (uintptr_t)&ret0 },
        { "glReadPixels", (uintptr_t)&glReadPixels },
#ifdef DEBUG_OPENGL
        { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage_hook },
        { "glRenderbufferStorageOES", (uintptr_t)&glRenderbufferStorage_hook },
#else
        { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
        { "glRenderbufferStorageOES", (uintptr_t)&glRenderbufferStorage },
#endif
        { "glRotatef", (uintptr_t)&glRotatef_sfp },
        { "glRotatex", (uintptr_t)&glRotatex },
        { "glSampleCoverage", (uintptr_t)&ret0 },
        { "glSampleCoveragex", (uintptr_t)&ret0 },
        { "glScalef", (uintptr_t)&glScalef_sfp },
        { "glScalex", (uintptr_t)&glScalex },
        { "glScissor", (uintptr_t)&glScissor },
        { "glShadeModel", (uintptr_t)&glShadeModel },
        { "glShaderSource", (uintptr_t)&glShaderSource_soloader },
        { "glStencilFunc", (uintptr_t)&glStencilFunc },
        { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
        { "glStencilMask", (uintptr_t)&glStencilMask },
        { "glStencilOp", (uintptr_t)&glStencilOp },
        { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
        { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer },
        { "glTexEnvf", (uintptr_t)&glTexEnvf_sfp },
        { "glTexEnvfv", (uintptr_t)&glTexEnvfv },
        { "glTexEnvi", (uintptr_t)&glTexEnvi },
        { "glTexEnviv", (uintptr_t)&ret0 },
        { "glTexEnvx", (uintptr_t)&glTexEnvx },
        { "glTexEnvxv", (uintptr_t)&glTexEnvxv },
        { "glTexGenfOES", (uintptr_t)&ret0 },
        { "glTexGenfvOES", (uintptr_t)&ret0 },
        { "glTexGeniOES", (uintptr_t)&ret0 },
        { "glTexGenivOES", (uintptr_t)&ret0 },
        { "glTexGenxOES", (uintptr_t)&ret0 },
        { "glTexGenxvOES", (uintptr_t)&ret0 },
        { "glTexImage2D", (uintptr_t)&glTexImage2D },
        { "glTexParameterf", (uintptr_t)&glTexParameterf_sfp },
        { "glTexParameterfv", (uintptr_t)&ret0 },
        { "glTexParameteri", (uintptr_t)&glTexParameteri },
        { "glTexParameteriv", (uintptr_t)&glTexParameteriv },
        { "glTexParameterx", (uintptr_t)&glTexParameterx },
        { "glTexParameterxv", (uintptr_t)&ret0 },
        { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
        { "glTranslatef", (uintptr_t)&glTranslatef_sfp },
        { "glTranslatex", (uintptr_t)&glTranslatex },
        { "glUniform1f", (uintptr_t)&glUniform1f_sfp },
        { "glUniform1fv", (uintptr_t)&glUniform1fv },
#ifdef DEBUG_OPENGL
        { "glUniform1i", (uintptr_t)&glUniform1i_hook },
#else
        { "glUniform1i", (uintptr_t)&glUniform1i },
#endif
        { "glUniform1iv", (uintptr_t)&glUniform1iv },
        { "glUniform2i", (uintptr_t)&glUniform2i },
        { "glUniform2f", (uintptr_t)&glUniform2f_sfp },
        { "glUniform2fv", (uintptr_t)&glUniform2fv },
        { "glUniform2iv", (uintptr_t)&glUniform2iv },
        { "glUniform3i", (uintptr_t)&glUniform3i },
        { "glUniform3f", (uintptr_t)&glUniform3f_sfp },
        { "glUniform3fv", (uintptr_t)&glUniform3fv },
        { "glUniform3iv", (uintptr_t)&glUniform3iv },
        { "glUniform4i", (uintptr_t)&glUniform4i },
        { "glUniform4f", (uintptr_t)&glUniform4f_sfp },
        { "glUniform4fv", (uintptr_t)&glUniform4fv },
        { "glUniform4iv", (uintptr_t)&glUniform4iv },
        { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
        { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
#ifdef DEBUG_OPENGL
        { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv_hook },
#else
        { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
#endif
        { "glUnmapBuffer", (uintptr_t)&glUnmapBuffer },
        { "glUnmapBufferOES", (uintptr_t)&glUnmapBuffer },
#ifdef DEBUG_OPENGL
        { "glUseProgram", (uintptr_t)&glUseProgram_hook },
#else
        { "glUseProgram", (uintptr_t)&glUseProgram_fix },
#endif
        { "glValidateProgram", (uintptr_t)&ret0 },
#ifdef DEBUG_OPENGL
        { "glVertexAttrib1f", (uintptr_t)&glVertexAttrib1f_sfp_log },
        { "glVertexAttrib2f", (uintptr_t)&glVertexAttrib2f_sfp_log },
#else
        { "glVertexAttrib1f", (uintptr_t)&glVertexAttrib1f_sfp_fix },
        { "glVertexAttrib2f", (uintptr_t)&glVertexAttrib2f_sfp },
#endif
#ifdef DEBUG_OPENGL
        { "glVertexAttrib4f", (uintptr_t)&glVertexAttrib4f_sfp_log },
#else
        { "glVertexAttrib4f", (uintptr_t)&glVertexAttrib4f_sfp },
#endif
        { "glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv },
#ifdef DEBUG_OPENGL
        { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer_hook },
#else
        { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer_fix },
#endif
        { "glVertexPointer", (uintptr_t)&glVertexPointer },
#ifdef DEBUG_OPENGL
        { "glViewport", (uintptr_t)&glViewport_hook },
#else
        { "glViewport", (uintptr_t)&glViewport },
#endif
        { "glWeightPointerOES", (uintptr_t)&ret0 },


        // OpenSLES
        { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION },
        { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
        { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
        { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
        { "SL_IID_METADATAEXTRACTION", (uintptr_t)&SL_IID_METADATAEXTRACTION },
        { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
        { "SL_IID_PREFETCHSTATUS", (uintptr_t)&SL_IID_PREFETCHSTATUS },
        { "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD },
        { "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
        { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
        { "slCreateEngine", (uintptr_t)&slCreateEngine },


        // Pthread
        { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
        { "pthread_attr_init", (uintptr_t) &pthread_attr_init_soloader },
        { "pthread_attr_setdetachstate", (uintptr_t) &pthread_attr_setdetachstate_soloader },
        { "pthread_attr_setstacksize", (uintptr_t) &pthread_attr_setstacksize_soloader },
        { "pthread_attr_setschedparam", (uintptr_t) &ret0 },

        { "pthread_cond_broadcast", (uintptr_t) &pthread_cond_broadcast_soloader },
        { "pthread_cond_destroy", (uintptr_t) &pthread_cond_destroy_soloader },
        { "pthread_cond_init", (uintptr_t) &pthread_cond_init_soloader },
        { "pthread_cond_signal", (uintptr_t) &pthread_cond_signal_soloader },
        { "pthread_cond_timedwait", (uintptr_t) &pthread_cond_timedwait_soloader },
        { "pthread_cond_wait", (uintptr_t) &pthread_cond_wait_soloader },

        { "pthread_create", (uintptr_t) &pthread_create_soloader },
        { "pthread_detach", (uintptr_t) &pthread_detach_soloader },
        { "pthread_equal", (uintptr_t) &pthread_equal_soloader },
        { "pthread_exit", (uintptr_t)&pthread_exit },
        { "pthread_getschedparam", (uintptr_t) &pthread_getschedparam_soloader },
        { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
        { "pthread_join", (uintptr_t) &pthread_join_soloader },
        { "pthread_key_create", (uintptr_t)&pthread_key_create },
        { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
        { "pthread_kill", (uintptr_t)&pthread_kill_soloader },

        { "pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_soloader },
        { "pthread_mutex_init", (uintptr_t) &pthread_mutex_init_soloader },
        { "pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_soloader },
        { "pthread_mutex_trylock", (uintptr_t) &pthread_mutex_trylock_soloader },
        { "pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_soloader },
        { "pthread_mutexattr_destroy", (uintptr_t) &pthread_mutexattr_destroy_soloader },
        { "pthread_mutexattr_init", (uintptr_t) &pthread_mutexattr_init_soloader },
        { "pthread_mutexattr_settype", (uintptr_t) &pthread_mutexattr_settype_soloader },
        { "pthread_mutexattr_setpshared", (uintptr_t) &ret0 },
        { "pthread_once", (uintptr_t)&pthread_once_soloader },

        { "pthread_self", (uintptr_t) &pthread_self_soloader },
        { "pthread_setname_np", (uintptr_t) &pthread_setname_np_soloader },
        { "pthread_setschedparam", (uintptr_t) &pthread_setschedparam_soloader },
        { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
        { "pthread_sigmask", (uintptr_t)&ret0 },

        { "sem_destroy", (uintptr_t) &sem_destroy_soloader },
        { "sem_getvalue", (uintptr_t) &sem_getvalue_soloader },
        { "sem_init", (uintptr_t) &sem_init_soloader },
        { "sem_post", (uintptr_t) &sem_post_soloader },
        { "sem_timedwait", (uintptr_t) &sem_timedwait_soloader },
        { "sem_trywait", (uintptr_t) &sem_trywait_soloader },
        { "sem_wait", (uintptr_t) &sem_wait_soloader },

        { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max },
        { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min },
        { "sched_yield", (uintptr_t)&sched_yield },


        // wchar, wctype
        { "btowc", (uintptr_t)&btowc },
        { "iswalpha", (uintptr_t)&iswalpha },
        { "iswcntrl", (uintptr_t)&iswcntrl },
        { "iswctype", (uintptr_t)&iswctype },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswdigit", (uintptr_t)&iswdigit },
        { "iswlower", (uintptr_t)&iswlower },
        { "iswprint", (uintptr_t)&iswprint },
        { "iswpunct", (uintptr_t)&iswpunct },
        { "iswspace", (uintptr_t)&iswspace },
        { "iswupper", (uintptr_t)&iswupper },
        { "iswxdigit", (uintptr_t)&iswxdigit },
        { "mbrlen", (uintptr_t)&mbrlen },
        { "mbrtowc", (uintptr_t)&mbrtowc },
        { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs },
        { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
        { "mbstowcs", (uintptr_t)&mbstowcs },
        { "mbtowc", (uintptr_t)&mbtowc },
        { "towlower", (uintptr_t)&towlower },
        { "towupper", (uintptr_t)&towupper },
        { "wcrtomb", (uintptr_t)&wcrtomb },
        { "wcscasecmp", (uintptr_t)&wcscasecmp },
        { "wcscmp", (uintptr_t)&wcscmp },
        { "wcscoll", (uintptr_t)&wcscoll },
        { "wcscpy", (uintptr_t)&wcscpy },
        { "wcsftime", (uintptr_t)&wcsftime },
        { "wcslcat", (uintptr_t)&wcslcat },
        { "wcslcpy", (uintptr_t)&wcslcpy },
        { "wcslen", (uintptr_t)&wcslen },
        { "wcsncasecmp", (uintptr_t)&wcsncasecmp },
        { "wcsncmp", (uintptr_t)&wcsncmp },
        { "wcsncpy", (uintptr_t)&wcsncpy },
        { "wcsnlen", (uintptr_t)&wcsnlen },
        { "wcsnrtombs", (uintptr_t)&wcsnrtombs },
        { "wcsstr", (uintptr_t)&wcsstr },
        { "wcstod", (uintptr_t)&wcstod },
        { "wcstof", (uintptr_t)&wcstof },
        { "wcstol", (uintptr_t)&wcstol },
        { "wcstoll", (uintptr_t)&wcstoll },
        { "wcstombs", (uintptr_t)&wcstombs },
        { "wcstoul", (uintptr_t)&wcstoul },
        { "wcstoull", (uintptr_t)&wcstoull },
        { "wcsxfrm", (uintptr_t)&wcsxfrm },
        { "wctob", (uintptr_t)&wctob },
        { "wctype", (uintptr_t)&wctype },
        { "wmemchr", (uintptr_t)&wmemchr },
        { "wmemcmp", (uintptr_t)&wmemcmp },
        { "wmemcpy", (uintptr_t)&wmemcpy },
        { "wmemmove", (uintptr_t)&wmemmove },
        { "wmemset", (uintptr_t)&wmemset },


        // libdl
        { "dladdr", (uintptr_t)&ret0 },
        { "dlclose", (uintptr_t)&dlclose_soloader },
        { "dlerror", (uintptr_t)&dlerror_soloader },
        { "dlopen", (uintptr_t)&dlopen_soloader },
        { "dlsym", (uintptr_t)&dlsym_soloader },


        // Errno
        { "__errno", (uintptr_t)&__errno_soloader },
        { "strerror", (uintptr_t)&strerror_soloader },
        { "strerror_r", (uintptr_t)&strerror_r_soloader },
        { "perror", (uintptr_t)&perror }, // TODO: errno translation


        // Strings
        { "memchr", (uintptr_t)&memchr },
        { "memrchr", (uintptr_t)&memrchr },
        { "strcasecmp", (uintptr_t)&strcasecmp },
        { "strcat", (uintptr_t)&strcat },
        { "strchr", (uintptr_t)&strchr },
        { "strcmp", (uintptr_t)&strcmp },
        { "strcoll", (uintptr_t)&strcoll },
        { "strcpy", (uintptr_t)&strcpy },
        { "strcspn", (uintptr_t)&strcspn },
        { "strdup", (uintptr_t)&strdup },
        { "strlcat", (uintptr_t)&strlcat },
        { "strlcpy", (uintptr_t)&strlcpy },
        { "strlen", (uintptr_t)&strlen },
        { "strncasecmp", (uintptr_t)&strncasecmp },
        { "strncat", (uintptr_t)&strncat },
        { "strncmp", (uintptr_t)&strncmp },
        { "strncpy", (uintptr_t)&strncpy },
        { "strnlen", (uintptr_t)&strnlen },
        { "strpbrk", (uintptr_t)&strpbrk },
        { "strrchr", (uintptr_t)&strrchr },
        { "strspn", (uintptr_t)&strspn },
        { "strstr", (uintptr_t)&strstr },
        { "strtok", (uintptr_t)&strtok },
        { "strtok_r", (uintptr_t)&strtok_r },
        { "strxfrm", (uintptr_t)&strxfrm },


        // Syscalls
        { "fork", (uintptr_t)&fork },
        { "getpagesize", (uintptr_t)&getpagesize },
        { "getpid", (uintptr_t)&getpid },
        { "sbrk", (uintptr_t)&sbrk },
        { "syscall", (uintptr_t)&syscall },
        { "setpriority", (uintptr_t)&ret0 },
        { "sysconf", (uintptr_t)&ret0 },
        { "system", (uintptr_t)&system },
        { "umask", (uintptr_t)&ret0 },
        { "waitpid", (uintptr_t)&ret0 },


        // Time
        { "clock", (uintptr_t)&clock_soloader },
        { "clock_getres", (uintptr_t)&clock_getres_soloader },
        { "clock_gettime", (uintptr_t)&clock_gettime_soloader },
        { "difftime", (uintptr_t)&difftime_sfp },
        { "gettimeofday", (uintptr_t)&gettimeofday },
        { "gmtime", (uintptr_t)&gmtime },
        { "gmtime64", (uintptr_t)&gmtime64 },
        { "gmtime_r", (uintptr_t)&gmtime_r },
        { "localtime", (uintptr_t)&localtime },
        { "localtime64", (uintptr_t)&localtime64 },
        { "localtime_r", (uintptr_t)&localtime_r },
        { "mktime", (uintptr_t)&mktime },
        { "mktime64", (uintptr_t)&mktime64 },
        { "nanosleep", (uintptr_t)&nanosleep },
        { "strftime", (uintptr_t)&strftime },
        { "time", (uintptr_t)&time },
        { "__libc_current_sigrtmin", (uintptr_t)&ret0 },
        { "timer_create", (uintptr_t)&timer_create_soloader },
        { "timer_delete", (uintptr_t)&timer_delete_soloader },
        { "timer_settime", (uintptr_t)&timer_settime_soloader },
        { "tzset", (uintptr_t)&tzset },


        // Temp
        { "mkstemp", (uintptr_t)&mkstemp },
        { "mktemp", (uintptr_t)&mktemp },
        { "tmpfile", (uintptr_t)&tmpfile },
        { "tmpnam", (uintptr_t)&tmpnam },


        // stdlib
        { "abort", (uintptr_t)&abort_soloader },
        { "atof", (uintptr_t)&atof_sfp },
        { "atoi", (uintptr_t)&atoi },
        { "atol", (uintptr_t)&atol },
        { "atoll", (uintptr_t)&atoll },
        { "bsearch", (uintptr_t)&bsearch },
        { "exit", (uintptr_t)&exit_soloader },
        { "lrand48", (uintptr_t)&lrand48 },
        { "getauxval", (uintptr_t)&getauxval_soloader },
        { "prctl", (uintptr_t)&ret0 },
        { "sleep", (uintptr_t)&sleep },
        { "srand48", (uintptr_t)&srand48 },
        { "strtod", (uintptr_t)&strtod_sfp },
        { "strtof", (uintptr_t)&strtof_sfp },
        { "strtoimax", (uintptr_t)&strtoimax },
        { "strtol", (uintptr_t)&strtol },
        { "strtold", (uintptr_t)&strtold_sfp },
        { "strtoll", (uintptr_t)&strtoll },
        { "strtoul", (uintptr_t)&strtoul },
        { "strtoull", (uintptr_t)&strtoull },
        { "strtoumax", (uintptr_t)&strtoumax },
        { "usleep", (uintptr_t)&usleep },

        #ifdef USE_SCELIBC_IO
            { "qsort", (uintptr_t)&sceLibcBridge_qsort },
            { "rand", (uintptr_t)&sceLibcBridge_rand },
            { "srand", (uintptr_t)&sceLibcBridge_srand },
        #else
            { "qsort", (uintptr_t)&qsort },
            { "rand", (uintptr_t)&rand },
            { "srand", (uintptr_t)&srand },
        #endif


        // Env
        { "getenv", (uintptr_t)&getenv_soloader },
        { "setenv", (uintptr_t)&setenv_soloader },


        // Jmp
        { "setjmp", (uintptr_t)&setjmp }, // TODO: May have different struct size?
        { "longjmp", (uintptr_t)&longjmp }, // TODO: May have different struct size?


        // Signals
        { "bsd_signal", (uintptr_t)&signal },
        { "raise", (uintptr_t)&raise },
        { "sigaction", (uintptr_t)&sigaction },


        // Locale
        { "freelocale", (uintptr_t)&freelocale },
        { "localeconv", (uintptr_t)&localeconv },
        { "newlocale", (uintptr_t)&newlocale },
        { "setlocale", (uintptr_t)&setlocale },
        { "uselocale", (uintptr_t)&uselocale },


        // zlib
        { "adler32", (uintptr_t)&adler32 },
        { "compress", (uintptr_t)&compress },
        { "compressBound", (uintptr_t)&compressBound },
        { "crc32", (uintptr_t)&crc32 },
        { "deflate", (uintptr_t)&deflate },
        { "deflateEnd", (uintptr_t)&deflateEnd },
        { "deflateInit2_", (uintptr_t)&deflateInit2_ },
        { "deflateInit_", (uintptr_t)&deflateInit_ },
        { "deflateReset", (uintptr_t)&deflateReset },
        { "gzclose", (uintptr_t)&gzclose },
        { "gzgets", (uintptr_t)&gzgets },
        { "gzopen", (uintptr_t)&gzopen },
        { "inflate", (uintptr_t)&inflate },
        { "inflateEnd", (uintptr_t)&inflateEnd },
        { "inflateInit2_", (uintptr_t)&inflateInit2_ },
        { "inflateInit_", (uintptr_t)&inflateInit_ },
        { "inflateReset", (uintptr_t)&inflateReset },
        { "inflateReset2", (uintptr_t)&inflateReset2 },
        { "uncompress", (uintptr_t)&uncompress },
};

void *dlsym_soloader(void * handle, const char * symbol) {
    (void)handle;
    if (!symbol) {
        dlerror_set_soloader("dlsym: NULL symbol name.");
        l_error("dlsym: NULL symbol name.");
        return NULL;
    }
    for (int i = 0; i < sizeof(default_dynlib) / sizeof(default_dynlib[0]); i++) {
        if (strcmp(symbol, default_dynlib[i].symbol) == 0) {
            dlerror_clear_soloader();
            return (void *)default_dynlib[i].func;
        }
    }

    char msg[192];
    sceClibSnprintf(msg, sizeof(msg), "dlsym: Unknown symbol \"%s\".", symbol ? symbol : "(null)");
    dlerror_set_soloader(msg);
    l_error("dlsym: Unknown symbol \"%s\".", symbol);
    return NULL;
}

void resolve_imports(so_module* mod) {
    __sF_fake[0] = *stdin;
    __sF_fake[1] = *stdout;
    __sF_fake[2] = *stderr;

    so_resolve(mod, default_dynlib, sizeof(default_dynlib), 0);
}
