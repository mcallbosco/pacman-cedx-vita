/*
 * Background file preloader — see preloader.h.
 *
 * The cache is a fixed-size list of (path, buffer, size) entries populated
 * by a background thread. preloader_try_open(path) matches the path, and
 * on a hit returns fmemopen(buffer, size, "rb") — a REAL newlib FILE*
 * backed by our in-memory buffer. This means code that bypasses our
 * soloader shims (notably FreeType, which does raw newlib stdio on the
 * FILE* it's handed) sees a valid __sFILE and works normally.
 *
 * Warmup population:
 *   - Explicit entries (e.g. RR_font.ttf) listed at build time.
 *   - Auto-scan of asset directories (e.g. assets/sound/) for bulk
 *     inclusion without hardcoding dozens of paths.
 *
 * Thread priority is lower than the main thread so we're purely
 * opportunistic: if the main thread isn't reading the stick, we are.
 * If the main thread asks for a file we haven't finished reading yet,
 * preloader_try_open returns NULL and the caller falls back to real
 * fopen — no blocking.
 */

#include "preloader.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/dirent.h>

#include "logger.h"

#ifndef DATA_PATH
#define DATA_PATH "app0:data/"
#endif

#define PRELOAD_MAX_ENTRIES 128
#define PRELOAD_PATH_LEN    160

typedef struct {
    char   path[PRELOAD_PATH_LEN];
    void  *data;
    size_t size;
    volatile int loaded;     /* 0 until data is fully read, 1 after */
    volatile int reserved;   /* slot is claimed (either loading or loaded) */
} preload_entry_t;

static preload_entry_t g_cache[PRELOAD_MAX_ENTRIES];
static int g_cache_count = 0;
static volatile int g_preloader_started = 0;

/* ---- Opened-handle tracking ----
 * fmemopen returns a newlib FILE*, but the rest of the project routes
 * fread/fseek/fclose through sceLibcBridge when USE_SCELIBC_IO is set.
 * Those two stdio implementations have incompatible __sFILE layouts, so
 * the io.c shims need to know which path a given FILE* belongs on. This
 * table is the truth source: if fp is here, use newlib. */
#define PRELOAD_OPEN_MAX 32
static FILE *g_opened[PRELOAD_OPEN_MAX];
/* Parallel to g_opened. If g_slurp_data[i] != NULL, this is a slurp handle
 * — we own the buffer and know its size, so io.c can service
 * fread/fseek/ftell via direct memcpy without any newlib call at all.
 * Preloader-cache hits (currently disabled) leave g_slurp_data NULL: the
 * cache owns the buffer in that case and the handle is a plain fmemopen
 * FILE* routed through newlib. */
static void  *g_slurp_data[PRELOAD_OPEN_MAX];
static size_t g_slurp_size[PRELOAD_OPEN_MAX];
static size_t g_slurp_pos[PRELOAD_OPEN_MAX];

static void track_add(FILE *fp) {
    for (int i = 0; i < PRELOAD_OPEN_MAX; ++i) {
        if (!g_opened[i]) {
            g_opened[i] = fp;
            g_slurp_data[i] = NULL;
            g_slurp_size[i] = 0;
            g_slurp_pos[i] = 0;
            return;
        }
    }
    l_warn("[PRELOAD] open-tracking full, fp=%p will route wrong", fp);
}

static void track_add_owned(FILE *fp, void *owned, size_t size) {
    for (int i = 0; i < PRELOAD_OPEN_MAX; ++i) {
        if (!g_opened[i]) {
            g_opened[i] = fp;
            g_slurp_data[i] = owned;
            g_slurp_size[i] = size;
            g_slurp_pos[i] = 0;
            return;
        }
    }
    l_warn("[PRELOAD] open-tracking full, fp=%p will route wrong (and leak %p)", fp, owned);
}

static void track_remove(FILE *fp) {
    for (int i = 0; i < PRELOAD_OPEN_MAX; ++i) {
        if (g_opened[i] == fp) {
            if (g_slurp_data[i]) free(g_slurp_data[i]);
            g_opened[i] = NULL;
            g_slurp_data[i] = NULL;
            g_slurp_size[i] = 0;
            g_slurp_pos[i] = 0;
            return;
        }
    }
}

static int slurp_slot_of(FILE *fp) {
    if (!fp) return -1;
    for (int i = 0; i < PRELOAD_OPEN_MAX; ++i) {
        if (g_opened[i] == fp && g_slurp_data[i]) return i;
    }
    return -1;
}

int preloader_is_slurp(FILE *fp) {
    return slurp_slot_of(fp) >= 0;
}

size_t preloader_slurp_fast_read(FILE *fp, void *dst, size_t nbytes) {
    int i = slurp_slot_of(fp);
    if (i < 0) return 0;
    size_t remaining = g_slurp_size[i] - g_slurp_pos[i];
    size_t copy = nbytes < remaining ? nbytes : remaining;
    if (copy > 0 && dst) {
        memcpy(dst, (char *)g_slurp_data[i] + g_slurp_pos[i], copy);
        g_slurp_pos[i] += copy;
    }
    return copy;
}

int preloader_slurp_fast_seek(FILE *fp, long offset, int whence) {
    int i = slurp_slot_of(fp);
    if (i < 0) return -1;
    long new_pos;
    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = (long)g_slurp_pos[i] + offset; break;
        case SEEK_END: new_pos = (long)g_slurp_size[i] + offset; break;
        default: return -1;
    }
    if (new_pos < 0 || (size_t)new_pos > g_slurp_size[i]) return -1;
    g_slurp_pos[i] = (size_t)new_pos;
    return 0;
}

long preloader_slurp_fast_tell(FILE *fp) {
    int i = slurp_slot_of(fp);
    if (i < 0) return -1;
    return (long)g_slurp_pos[i];
}

int preloader_is_preloaded(FILE *fp) {
    if (!fp) return 0;
    for (int i = 0; i < PRELOAD_OPEN_MAX; ++i) {
        if (g_opened[i] == fp) return 1;
    }
    return 0;
}

void preloader_fclose_tracked(FILE *fp) {
    track_remove(fp);
}

/* ---- Warmup list population ---- */

static int add_entry(const char *path) {
    if (g_cache_count >= PRELOAD_MAX_ENTRIES) return 0;
    if (!path || strlen(path) >= PRELOAD_PATH_LEN) return 0;
    preload_entry_t *e = &g_cache[g_cache_count++];
    strncpy(e->path, path, PRELOAD_PATH_LEN - 1);
    e->path[PRELOAD_PATH_LEN - 1] = '\0';
    e->data = NULL;
    e->size = 0;
    e->loaded = 0;
    e->reserved = 1;
    return 1;
}

/* Scan dir for files matching *ext. Skips names starting with `skip_prefix`
 * (pass NULL to skip nothing) and files bigger than max_bytes (pass 0 to
 * skip the size check — requires an extra stat per entry). The size check
 * is the safety net that keeps us from accidentally preloading 3-5 MB BGM
 * tracks that are streamed, not read whole. */
static void scan_dir(const char *dir, const char *ext,
                     const char *skip_prefix, size_t max_bytes) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) {
        l_warn("[PRELOAD] scan_dir open failed: %s (0x%08X)", dir, d);
        return;
    }
    size_t extlen = strlen(ext);
    size_t skip_len = skip_prefix ? strlen(skip_prefix) : 0;
    SceIoDirent ent;
    int added = 0, skipped = 0;
    while (sceIoDread(d, &ent) > 0) {
        if (ent.d_name[0] == '.') continue;
        size_t nlen = strlen(ent.d_name);
        if (nlen < extlen) continue;
        if (strcasecmp(ent.d_name + nlen - extlen, ext) != 0) continue;
        if (skip_len && nlen >= skip_len &&
            strncasecmp(ent.d_name, skip_prefix, skip_len) == 0) {
            skipped++;
            continue;
        }
        if (max_bytes && (size_t)ent.d_stat.st_size > max_bytes) {
            skipped++;
            continue;
        }
        char full[PRELOAD_PATH_LEN];
        int n = snprintf(full, sizeof(full), "%s%s", dir, ent.d_name);
        if (n <= 0 || n >= (int)sizeof(full)) continue;
        if (add_entry(full)) added++;
    }
    sceIoDclose(d);
    l_info("[PRELOAD] scan %s*%s: +%d entries (skipped %d)",
           dir, ext, added, skipped);
}

static void populate_warmup(void) {
    /* RR_font.ttf — biggest pure-I/O file in the startup path (~1.4 s
     * fread for 14 MB). FreeType uses FT_Stream-style function pointers
     * so it's tolerant of the fmemopen-returned newlib FILE*.
     *
     * Note: .ogg preloading was tried and broke — the game's OGG decoder
     * (libvorbis, statically linked in the .so) does inline stdio access
     * on the FILE* assuming sceLibc's __sFILE layout. We can't produce a
     * sceLibc-layout FILE* from a memory buffer (SceLibcBridge exposes no
     * fmemopen/funopen), so OGGs must stay on the native fopen path. */
    add_entry(DATA_PATH "assets/data/RR_font.ttf");
}

/* ---- Background read thread ---- */

static int preloader_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    size_t total_bytes = 0;
    int loaded_count = 0;

    for (int i = 0; i < g_cache_count; ++i) {
        preload_entry_t *e = &g_cache[i];
        FILE *f = fopen(e->path, "rb");
        if (!f) {
            /* Not an error — file may not exist on all builds. */
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long sz = ftell(f);
        if (sz <= 0) { fclose(f); continue; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); continue; }
        void *buf = malloc((size_t)sz);
        if (!buf) { fclose(f); continue; }
        size_t got = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (got != (size_t)sz) {
            l_warn("[PRELOAD] short read: %s (%zu/%ld)", e->path, got, sz);
            free(buf);
            continue;
        }
        e->data = buf;
        e->size = (size_t)sz;
        __sync_synchronize();
        e->loaded = 1;
        total_bytes += (size_t)sz;
        loaded_count++;
    }

    l_info("[PRELOAD] done: %d/%d files, %zu KB, %llu ms",
           loaded_count, g_cache_count, total_bytes / 1024,
           (unsigned long long)((sceKernelGetProcessTimeWide() - t0) / 1000));
    return 0;
}

void preloader_start(void) {
    if (g_preloader_started) return;
    g_preloader_started = 1;

    populate_warmup();
    l_info("[PRELOAD] warmup list: %d entries", g_cache_count);

    SceUID thid = sceKernelCreateThread("preloader",
                                        preloader_thread,
                                        0x10000100, /* lower prio than main */
                                        0x20000,    /* 128 KB stack */
                                        0, 0, NULL);
    if (thid < 0) {
        l_warn("[PRELOAD] create_thread failed: 0x%08X", thid);
        return;
    }
    int r = sceKernelStartThread(thid, 0, NULL);
    if (r < 0) {
        l_warn("[PRELOAD] start_thread failed: 0x%08X", r);
        return;
    }
}

FILE *preloader_slurp_adopt(void *buf, size_t size) {
    if (!buf || size == 0) {
        if (buf) free(buf);
        return NULL;
    }
    FILE *fp = fmemopen(buf, size, "rb");
    if (!fp) {
        free(buf);
        return NULL;
    }
    track_add_owned(fp, buf, size);
    return fp;
}

FILE *preloader_try_open(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < g_cache_count; ++i) {
        preload_entry_t *e = &g_cache[i];
        if (strcmp(e->path, path) != 0) continue;
        __sync_synchronize();
        if (!e->loaded) {
            /* Still reading — fall through to the real fopen path rather
             * than block. If the preloader is racing us to the same file,
             * waiting buys nothing. */
            return NULL;
        }
        /* fmemopen returns a real newlib FILE*, so the caller (and any
         * library that bypasses our shims) can use it directly. */
        FILE *fp = fmemopen(e->data, e->size, "rb");
        if (fp) track_add(fp);
        return fp;
    }
    return NULL;
}
