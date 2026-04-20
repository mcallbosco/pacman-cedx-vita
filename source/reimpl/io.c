/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/preloader.h"
#include "utils/pgxt.h"
#include <psp2/kernel/processmgr.h>

/* ===== Profiling counters (shared with java.c) =====
 * Counter storage always lives in java.c (~88 bytes BSS) so externally
 * visible symbols stay linkable; per-call increments and the expensive
 * bookkeeping are gated by ENABLE_IO_PROFILING below. */
extern uint64_t g_prof_open_count;
extern uint64_t g_prof_open_us;
extern uint64_t g_prof_read_count;
extern uint64_t g_prof_read_bytes;
extern uint64_t g_prof_read_us;
extern uint64_t g_prof_png_bytes;
extern uint64_t g_prof_png_us;
extern uint64_t g_prof_ogg_bytes;
extern uint64_t g_prof_ogg_us;
extern uint64_t g_prof_other_bytes;
extern uint64_t g_prof_other_us;

static int prof_ends_with(const char *s, const char *ext) {
    if (!s || !ext) return 0;
    size_t sl = strlen(s), el = strlen(ext);
    if (sl < el) return 0;
    for (size_t i = 0; i < el; ++i) {
        char a = s[sl - el + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

#ifdef ENABLE_IO_PROFILING

/* Category per open FILE*: 0=other, 1=png, 2=ogg */
#define PROF_FILE_SLOTS 64
#define PROF_PATH_LEN   96
static FILE   *g_prof_file_ptrs[PROF_FILE_SLOTS];
static int     g_prof_file_cats[PROF_FILE_SLOTS];
static char    g_prof_file_paths[PROF_FILE_SLOTS][PROF_PATH_LEN];
static uint64_t g_prof_file_open_us[PROF_FILE_SLOTS];
static uint64_t g_prof_file_fread_us[PROF_FILE_SLOTS];
static uint64_t g_prof_file_fread_bytes[PROF_FILE_SLOTS];
static uint32_t g_prof_file_fread_calls[PROF_FILE_SLOTS];

int g_prof_timeline = 0;
static uint64_t g_prof_timeline_t0 = 0;
static uint64_t g_prof_last_close_us = 0;
static char     g_prof_last_close_path[PROF_PATH_LEN] = {0};
static int      g_prof_last_close_cat = 0;

void prof_timeline_enable(void) {
    g_prof_timeline = 1;
    g_prof_timeline_t0 = sceKernelGetProcessTimeWide();
}

void prof_timeline_disable(void) {
    g_prof_timeline = 0;
}

static const char *prof_short_path(const char *path) {
    if (!path) return "?";
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

static void prof_register_file(FILE *fp, const char *path) {
    if (!fp || !path) return;
    int cat = 0;
    if (prof_ends_with(path, ".png")) cat = 1;
    else if (prof_ends_with(path, ".ogg") || prof_ends_with(path, ".mp3")) cat = 2;
    uint64_t now_us = sceKernelGetProcessTimeWide();
    for (int i = 0; i < PROF_FILE_SLOTS; ++i) {
        if (g_prof_file_ptrs[i] == NULL) {
            g_prof_file_ptrs[i] = fp;
            g_prof_file_cats[i] = cat;
            g_prof_file_open_us[i] = now_us;
            g_prof_file_fread_us[i] = 0;
            g_prof_file_fread_bytes[i] = 0;
            g_prof_file_fread_calls[i] = 0;
            strncpy(g_prof_file_paths[i], prof_short_path(path), PROF_PATH_LEN - 1);
            g_prof_file_paths[i][PROF_PATH_LEN - 1] = '\0';
            if (g_prof_timeline) {
                uint64_t t_rel = (now_us - g_prof_timeline_t0) / 1000;
                uint64_t gap = 0;
                if (g_prof_last_close_us != 0) {
                    gap = (now_us - g_prof_last_close_us) / 1000;
                }
                if (gap > 20) {
                    l_info("[PROF-TL] +%5llu ms  GAP %4llu ms after %s",
                           (unsigned long long)t_rel,
                           (unsigned long long)gap,
                           g_prof_last_close_path[0] ? g_prof_last_close_path : "(start)");
                }
                l_info("[PROF-TL] +%5llu ms  OPEN  %s",
                       (unsigned long long)t_rel,
                       g_prof_file_paths[i]);
            }
            return;
        }
    }
}

static int prof_lookup_cat(FILE *fp) {
    for (int i = 0; i < PROF_FILE_SLOTS; ++i)
        if (g_prof_file_ptrs[i] == fp) return g_prof_file_cats[i];
    return 0;
}

static void prof_attribute_fread(FILE *fp, uint64_t dt_us, size_t got) {
    for (int i = 0; i < PROF_FILE_SLOTS; ++i) {
        if (g_prof_file_ptrs[i] == fp) {
            g_prof_file_fread_us[i] += dt_us;
            g_prof_file_fread_bytes[i] += (uint64_t)got;
            g_prof_file_fread_calls[i]++;
            return;
        }
    }
}

static void prof_unregister_file(FILE *fp) {
    for (int i = 0; i < PROF_FILE_SLOTS; ++i) {
        if (g_prof_file_ptrs[i] == fp) {
            if (g_prof_timeline) {
                uint64_t now_us = sceKernelGetProcessTimeWide();
                uint64_t t_rel = (now_us - g_prof_timeline_t0) / 1000;
                uint64_t lifetime = (now_us - g_prof_file_open_us[i]) / 1000;
                uint64_t fread_ms = g_prof_file_fread_us[i] / 1000;
                l_info("[PROF-TL] +%5llu ms  CLOSE %s (open_for=%llu ms fread=%llu ms bytes=%llu calls=%u)",
                       (unsigned long long)t_rel,
                       g_prof_file_paths[i],
                       (unsigned long long)lifetime,
                       (unsigned long long)fread_ms,
                       (unsigned long long)g_prof_file_fread_bytes[i],
                       (unsigned)g_prof_file_fread_calls[i]);
                g_prof_last_close_us = now_us;
                g_prof_last_close_cat = g_prof_file_cats[i];
                strncpy(g_prof_last_close_path, g_prof_file_paths[i], PROF_PATH_LEN - 1);
                g_prof_last_close_path[PROF_PATH_LEN - 1] = '\0';
            }
            g_prof_file_ptrs[i] = NULL;
            g_prof_file_cats[i] = 0;
            g_prof_file_paths[i][0] = '\0';
            g_prof_file_fread_us[i] = 0;
            g_prof_file_fread_bytes[i] = 0;
            g_prof_file_fread_calls[i] = 0;
            return;
        }
    }
}

#else /* !ENABLE_IO_PROFILING — no-op stubs so call sites stay clean */

void prof_timeline_enable(void)  {}
void prof_timeline_disable(void) {}
static inline void prof_register_file(FILE *fp, const char *path) { (void)fp; (void)path; }
static inline int  prof_lookup_cat(FILE *fp) { (void)fp; return 0; }
static inline void prof_attribute_fread(FILE *fp, uint64_t dt_us, size_t got) {
    (void)fp; (void)dt_us; (void)got;
}
static inline void prof_unregister_file(FILE *fp) { (void)fp; }

#endif /* ENABLE_IO_PROFILING */

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

#define BGM_TRACKED_FILES_MAX 64

typedef struct {
    FILE *fp;
    char path[256];
    uint64_t bytes_read;
    uint32_t read_calls;
    uint32_t seek_calls;
} bgm_file_track_t;

static bgm_file_track_t g_bgm_tracks[BGM_TRACKED_FILES_MAX];

static int is_bgm_path(const char *path) {
    return path && strstr(path, "sound/bgm");
}

static bgm_file_track_t *bgm_track_find(FILE *fp) {
    if (!fp)
        return NULL;
    for (int i = 0; i < BGM_TRACKED_FILES_MAX; ++i) {
        if (g_bgm_tracks[i].fp == fp)
            return &g_bgm_tracks[i];
    }
    return NULL;
}

static void bgm_track_open(FILE *fp, const char *path) {
    if (!fp || !is_bgm_path(path))
        return;

    bgm_file_track_t *slot = bgm_track_find(fp);
    if (!slot) {
        for (int i = 0; i < BGM_TRACKED_FILES_MAX; ++i) {
            if (!g_bgm_tracks[i].fp) {
                slot = &g_bgm_tracks[i];
                break;
            }
        }
    }
    if (!slot)
        return;

    memset(slot, 0, sizeof(*slot));
    slot->fp = fp;
    if (path) {
        snprintf(slot->path, sizeof(slot->path), "%s", path);
    }
    l_audio("[AUDIO][BGM] track open fp=%p path=%s", fp, slot->path);
}

static void bgm_track_read(FILE *fp, size_t requested, size_t got) {
    bgm_file_track_t *t = bgm_track_find(fp);
    if (!t)
        return;

    t->read_calls++;
    t->bytes_read += (uint64_t)got;
    if (t->read_calls <= 12 || got < requested || got == 0) {
        l_audio("[AUDIO][BGM] fread fp=%p req=%zu got=%zu total=%llu calls=%u path=%s",
                fp, requested, got, (unsigned long long)t->bytes_read,
                t->read_calls, t->path);
    }
}

static void bgm_track_seek(FILE *fp, long offset, int whence, int ret) {
    bgm_file_track_t *t = bgm_track_find(fp);
    if (!t)
        return;

    t->seek_calls++;
    if (t->seek_calls <= 12 || ret != 0) {
        l_audio("[AUDIO][BGM] fseek fp=%p off=%ld whence=%d ret=%d seeks=%u path=%s",
                fp, offset, whence, ret, t->seek_calls, t->path);
    }
}

static void bgm_track_close(FILE *fp, int fclose_ret) {
    bgm_file_track_t *t = bgm_track_find(fp);
    if (!t)
        return;

    l_audio("[AUDIO][BGM] close fp=%p ret=%d total=%llu reads=%u seeks=%u path=%s",
            fp, fclose_ret, (unsigned long long)t->bytes_read, t->read_calls,
            t->seek_calls, t->path);
    memset(t, 0, sizeof(*t));
}

/* ===== Negative directory cache =====
 *
 * The game brute-force probes maze-skin directories (~200 fopen calls for
 * files like skinA/pac_ce_maze05_mazeneon_L.bin that don't exist). Each
 * failed sceLibcBridge_fopen costs ~5–10 ms on Memory Stick, which adds up
 * to ~1–2 s of pure failed I/O.
 *
 * On first failed open in a directory we opendir() the whole directory
 * once, record every filename in a sorted basename list, and use that list
 * to short-circuit all future misses in the same directory. Hits (the file
 * really exists) also skip the scan once the listing is cached. */
#include <psp2/io/dirent.h>

#define NEG_CACHE_DIRS       16   /* number of distinct dirs we remember   */
#define NEG_CACHE_NAMES      256  /* max entries per dir listing           */
#define NEG_CACHE_NAME_LEN   64   /* max basename length                   */
#define NEG_CACHE_DIR_LEN    256  /* max full dir path length              */

typedef struct {
    char dir[NEG_CACHE_DIR_LEN];   /* full directory path, no trailing '/' */
    char names[NEG_CACHE_NAMES][NEG_CACHE_NAME_LEN];
    int  count;
    int  valid;
} neg_cache_entry_t;

static neg_cache_entry_t g_neg_cache[NEG_CACHE_DIRS];
static int g_neg_cache_next = 0;
static uint64_t g_neg_cache_hits = 0;
static uint64_t g_neg_cache_misses = 0;

/* Split path into dir + basename. Writes into caller's buffers. Returns 1
 * on success, 0 if path has no '/'. */
static int neg_split(const char *path, char *dir, size_t dir_sz,
                     const char **basename_out) {
    const char *slash = strrchr(path, '/');
    if (!slash) return 0;
    size_t dir_len = (size_t)(slash - path);
    if (dir_len >= dir_sz) return 0;
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';
    *basename_out = slash + 1;
    return 1;
}

static neg_cache_entry_t *neg_find(const char *dir) {
    for (int i = 0; i < NEG_CACHE_DIRS; ++i) {
        if (g_neg_cache[i].valid && strcmp(g_neg_cache[i].dir, dir) == 0)
            return &g_neg_cache[i];
    }
    return NULL;
}

/* Scan `dir` via sceIoDopen and populate a cache slot. */
static neg_cache_entry_t *neg_scan(const char *dir) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) {
        /* Directory doesn't exist — still cache an empty listing so we
         * don't keep retrying sceIoDopen on every probe. */
    }
    neg_cache_entry_t *e = &g_neg_cache[g_neg_cache_next];
    g_neg_cache_next = (g_neg_cache_next + 1) % NEG_CACHE_DIRS;
    memset(e, 0, sizeof(*e));
    strncpy(e->dir, dir, NEG_CACHE_DIR_LEN - 1);
    e->valid = 1;
    if (d >= 0) {
        SceIoDirent ent;
        while (e->count < NEG_CACHE_NAMES && sceIoDread(d, &ent) > 0) {
            if (ent.d_name[0] == '.') continue;
            strncpy(e->names[e->count], ent.d_name, NEG_CACHE_NAME_LEN - 1);
            e->names[e->count][NEG_CACHE_NAME_LEN - 1] = '\0';
            e->count++;
        }
        sceIoDclose(d);
    }
    return e;
}

static int neg_contains(const neg_cache_entry_t *e, const char *name) {
    if (!e) return 0;
    for (int i = 0; i < e->count; ++i) {
        if (strcmp(e->names[i], name) == 0) return 1;
    }
    return 0;
}

/* Return 1 if path is known-missing (caller should return NULL from fopen),
 * 0 if unknown or known-present. Lazy: populates cache on demand. */
static int neg_cache_check_missing(const char *path) {
    /* Only cache paths under our data root; paths elsewhere are rare and
     * not worth tracking. */
    if (strncmp(path, DATA_PATH_NOSLASH, strlen(DATA_PATH_NOSLASH)) != 0)
        return 0;

    char dir[NEG_CACHE_DIR_LEN];
    const char *base;
    if (!neg_split(path, dir, sizeof(dir), &base)) return 0;

    neg_cache_entry_t *e = neg_find(dir);
    if (!e) {
        e = neg_scan(dir);
    }
    if (!e || !e->valid) return 0;
    if (neg_contains(e, base)) return 0;  /* present — let fopen proceed */
    g_neg_cache_hits++;
    return 1;
}

void neg_cache_stats(uint64_t *hits, uint64_t *misses) {
    if (hits)   *hits = g_neg_cache_hits;
    if (misses) *misses = g_neg_cache_misses;
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    } else if (strncmp(filename, "/proc/", 6) == 0 ||
               strncmp(filename, "/sys/", 5) == 0) {
        /* Block access to Linux procfs/sysfs — return NULL (not found) */
        return NULL;
    }

    char remapped[512];

    /* Game code (libPacmanCE.so) constructs Android-style asset paths like
     * "/assets/data/skinA/...". On Vita that path doesn't exist, so every
     * such fopen used to cost ~10ms of sceLibcBridge_fopen probing a
     * nonexistent directory before the game retried with the real ux0 path.
     * Rewrite the prefix to DATA_PATH so the first call hits the right file
     * directly. Profiled at ~700 calls per cold start = ~3-4 seconds saved. */
    if (strncmp(filename, "/assets/", 8) == 0) {
        snprintf(remapped, sizeof(remapped), DATA_PATH "assets/%s",
                 filename + 8);
        filename = remapped;
    }

    /* Remap paths: the game sometimes constructs paths like
     * "ux0:data/pacmancedx/text/..." missing the "assets/" component.
     * Fix by inserting "assets/" after the DATA_PATH prefix. */
    if (strncmp(filename, DATA_PATH_NOSLASH, strlen(DATA_PATH_NOSLASH)) == 0 &&
        strncmp(filename + strlen(DATA_PATH), "assets/", 7) != 0 &&
        strncmp(filename + strlen(DATA_PATH), "files/", 6) != 0 &&
        strncmp(filename + strlen(DATA_PATH), "lib/", 4) != 0) {
        char remapped2[512];
        snprintf(remapped2, sizeof(remapped2), DATA_PATH "assets/%s",
                 filename + strlen(DATA_PATH));
        /* Copy back into the single remap buffer so filename survives. */
        memcpy(remapped, remapped2, sizeof(remapped));
        filename = remapped;
    }

#ifdef ENABLE_IO_PROFILING
    uint64_t _prof_t0 = sceKernelGetProcessTimeWide();
#endif
    FILE *ret = NULL;
    int only_read = (mode && (mode[0] == 'r') && !strchr(mode, '+'));

    /* Negative directory cache: short-circuit known-missing files before
     * we pay for a sceLibcBridge_fopen probe. Read-only paths only — write
     * opens create new files and must always go through to the real fopen. */
    if (only_read && neg_cache_check_missing(filename)) {
#ifdef ENABLE_IO_PROFILING
        g_prof_open_count++;
#endif
        l_debug("fopen(%s, %s): NULL [NEG-CACHE]", filename, mode);
        return NULL;
    }

    if (only_read) {
        ret = preloader_try_open(filename);
        if (ret) {
            /* Cache hit — no I/O cost. Still register in prof table so
             * timeline and category tracking work. */
#ifdef ENABLE_IO_PROFILING
            g_prof_open_count++;
#endif
            prof_register_file(ret, filename);
            pgxt_register_fopen_path(ret, filename);
            l_debug("fopen(%s, %s): %p [PRELOAD HIT]", filename, mode, ret);
            return ret;
        }
    }
#ifdef USE_SCELIBC_IO
    ret = sceLibcBridge_fopen(filename, mode);
#else
    ret = fopen(filename, mode);
#endif
#ifdef ENABLE_IO_PROFILING
    g_prof_open_us += sceKernelGetProcessTimeWide() - _prof_t0;
    g_prof_open_count++;
#endif
    if (ret) {
        prof_register_file(ret, filename);
        pgxt_register_fopen_path(ret, filename);
    }

    char final_bgm_path[512];
    final_bgm_path[0] = '\0';
    if (is_bgm_path(filename)) {
        snprintf(final_bgm_path, sizeof(final_bgm_path), "%s", filename);
    }

    if (!ret && strstr(filename, "bgm5_ost_pac_man_ce_")) {
        char alias_path[512];
        snprintf(alias_path, sizeof(alias_path), "%s", filename);
        char *needle = strstr(alias_path, "bgm5_ost_pac_man_ce_");
        if (needle) {
            const char *suffix = needle + strlen("bgm5_ost_pac_man_ce_");
            snprintf(needle, (size_t)(alias_path + sizeof(alias_path) - needle),
                            "bgm5_ost-pac-man-ce_%s", suffix);
#ifdef USE_SCELIBC_IO
            ret = sceLibcBridge_fopen(alias_path, mode);
#else
            ret = fopen(alias_path, mode);
#endif
            l_audio("[AUDIO][BGM] alias fopen(%s -> %s, %s): %p",
                    filename, alias_path, mode, ret);
            if (ret && is_bgm_path(alias_path)) {
                snprintf(final_bgm_path, sizeof(final_bgm_path), "%s", alias_path);
            }
        }
    }

    if (strstr(filename, "sound/bgm")) {
        l_audio("[AUDIO][BGM] fopen(%s, %s): %p", filename, mode, ret);
    }

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    if (ret && final_bgm_path[0] != '\0') {
        bgm_track_open(ret, final_bgm_path);
    }

    /* .runnybin slurp: these files are read by the game in tight
     * fread-per-glyph/animation-frame loops (menu.runnybin = 234k fread
     * calls for 650 KB). Replace the real FILE* with an fmemopen'd buffer
     * so those calls become memcpy instead of stdio. fmemopen returns a
     * newlib FILE*, so fread_soloader/fseek_soloader/fclose_soloader
     * already route correctly via preloader_is_preloaded(). */
    if (ret && only_read && prof_ends_with(filename, ".runnybin")) {
        long sz = -1;
#ifdef USE_SCELIBC_IO
        if (sceLibcBridge_fseek(ret, 0, SEEK_END) == 0) {
            sz = sceLibcBridge_ftell(ret);
            sceLibcBridge_fseek(ret, 0, SEEK_SET);
        }
#else
        if (fseek(ret, 0, SEEK_END) == 0) {
            sz = ftell(ret);
            fseek(ret, 0, SEEK_SET);
        }
#endif
        /* 4 MB cap — the real files are sub-MB; anything larger is a
         * corrupt ftell() or a file we shouldn't be slurping. */
        if (sz > 0 && sz < 4 * 1024 * 1024) {
            void *slurp_buf = malloc((size_t)sz);
            if (slurp_buf) {
                size_t got;
#ifdef USE_SCELIBC_IO
                got = sceLibcBridge_fread(slurp_buf, 1, (size_t)sz, ret);
#else
                got = fread(slurp_buf, 1, (size_t)sz, ret);
#endif
                if (got == (size_t)sz) {
                    /* Close the on-disk handle and replace with fmemopen. */
                    prof_unregister_file(ret);
#ifdef USE_SCELIBC_IO
                    sceLibcBridge_fclose(ret);
#else
                    fclose(ret);
#endif
                    FILE *mem_fp = preloader_slurp_adopt(slurp_buf, (size_t)sz);
                    if (mem_fp) {
                        prof_register_file(mem_fp, filename);
                        l_debug("fopen(%s, %s): %p [SLURP %ld B]", filename, mode, mem_fp, sz);
                        return mem_fp;
                    }
                    /* slurp_adopt freed slurp_buf and returned NULL. The
                     * original handle is already closed, so we must
                     * re-open to honor the caller's request. */
                    l_warn("[SLURP] fmemopen failed for %s; re-opening", filename);
#ifdef USE_SCELIBC_IO
                    ret = sceLibcBridge_fopen(filename, mode);
#else
                    ret = fopen(filename, mode);
#endif
                    if (ret) prof_register_file(ret, filename);
                } else {
                    free(slurp_buf);
                    /* fread fell short — rewind and fall back to the real
                     * FILE*, giving callers a fresh read position. */
#ifdef USE_SCELIBC_IO
                    sceLibcBridge_fseek(ret, 0, SEEK_SET);
#else
                    fseek(ret, 0, SEEK_SET);
#endif
                    l_warn("[SLURP] short read %zu/%ld for %s; falling back", got, sz, filename);
                }
            }
            /* malloc failure: leave ret untouched; it's already rewound. */
        }
    }

    return ret;
}

size_t fread_soloader(void *ptr, size_t size, size_t nmemb, FILE *stream) {
#ifdef ENABLE_IO_PROFILING
    uint64_t _prof_t0 = sceKernelGetProcessTimeWide();
#endif
    size_t ret;
    if (preloader_is_slurp(stream)) {
        /* Slurp handle — skip newlib entirely and memcpy from our buffer. */
        size_t req = size * nmemb;
        size_t got = preloader_slurp_fast_read(stream, ptr, req);
        ret = (size > 0) ? (got / size) : 0;
    } else if (preloader_is_preloaded(stream)) {
        /* fmemopen'd newlib FILE* that we don't own (preloader cache);
         * must use newlib fread to honor the __sFILE layout. */
        ret = fread(ptr, size, nmemb, stream);
    } else {
#ifdef USE_SCELIBC_IO
        ret = sceLibcBridge_fread(ptr, size, nmemb, stream);
#else
        ret = fread(ptr, size, nmemb, stream);
#endif
    }
    /* Track destination buffer for .png reads so cImage::LoadFromMem can
     * derive the source path. Whole-file reads (size=1, nmemb=filesize) are
     * typical; partial reads also register and will simply not match if the
     * game later passes a different buffer to LoadFromMem. */
    if (ret > 0) {
        pgxt_register_fread(stream, ptr, size * ret);
    }
#ifdef ENABLE_IO_PROFILING
    uint64_t _prof_dt = sceKernelGetProcessTimeWide() - _prof_t0;
    size_t got = size * ret;
    g_prof_read_count++;
    g_prof_read_bytes += (uint64_t)got;
    g_prof_read_us    += _prof_dt;
    int cat = prof_lookup_cat(stream);
    if (cat == 1) { g_prof_png_bytes += got; g_prof_png_us += _prof_dt; }
    else if (cat == 2) { g_prof_ogg_bytes += got; g_prof_ogg_us += _prof_dt; }
    else { g_prof_other_bytes += got; g_prof_other_us += _prof_dt; }
    prof_attribute_fread(stream, _prof_dt, got);
#endif
#ifdef ENABLE_AUDIO_LOGS
    {
        size_t req = size * nmemb;
        size_t got_bgm = size * ret;
        bgm_track_read(stream, req, got_bgm);
    }
#endif
    return ret;
}

int fseek_soloader(FILE *stream, long offset, int whence) {
    int ret;
    if (preloader_is_slurp(stream)) {
        ret = preloader_slurp_fast_seek(stream, offset, whence);
    } else if (preloader_is_preloaded(stream)) {
        ret = fseek(stream, offset, whence);
    } else {
#ifdef USE_SCELIBC_IO
        ret = sceLibcBridge_fseek(stream, offset, whence);
#else
        ret = fseek(stream, offset, whence);
#endif
    }
    bgm_track_seek(stream, offset, whence, ret);
    return ret;
}

long ftell_soloader(FILE *stream) {
    if (preloader_is_slurp(stream)) {
        return preloader_slurp_fast_tell(stream);
    }
    if (preloader_is_preloaded(stream)) {
        return ftell(stream);
    }
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_ftell(stream);
#else
    return ftell(stream);
#endif
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    } else if (strncmp(path, "/proc/", 6) == 0 ||
               strncmp(path, "/sys/", 5) == 0) {
        return -1;
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        l_debug("stat(%s): returning 0 in case this is a check for OpenSLES support", path);
        return 0;
    }

    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
    prof_unregister_file(f);
    pgxt_unregister_fopen(f);
    int ret;
    if (preloader_is_preloaded(f)) {
        preloader_fclose_tracked(f);
        ret = fclose(f);  /* newlib fclose for fmemopen'd handle */
    } else {
#ifdef USE_SCELIBC_IO
        ret = sceLibcBridge_fclose(f);
#else
        ret = fclose(f);
#endif
    }
    bgm_track_close(f, ret);
    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    DIR* ret = opendir(_pathname);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
