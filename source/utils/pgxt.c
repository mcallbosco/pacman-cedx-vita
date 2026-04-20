#include "utils/pgxt.h"
#include "utils/logger.h"

#include <kubridge.h>
#include <libc_bridge/libc_bridge.h>
#include <so_util/so_util.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif
extern so_module so_mod;
#ifdef __cplusplus
}
#endif

/* libpng types — opaque pointers; we never dereference them from here. */
typedef void *png_structp;
typedef void *png_infop;
typedef unsigned char *png_bytep;
typedef png_bytep *png_bytepp;

typedef size_t (*png_get_rowbytes_fn)(png_structp, png_infop);

/* ------------------------------------------------------------------ *
 * FILE* → full-path registry (populated by io.c on fopen)            *
 * ------------------------------------------------------------------ */

#define PGXT_FP_SLOTS   32
#define PGXT_PATH_MAX   256

static FILE *g_fp[PGXT_FP_SLOTS];
static char  g_fp_path[PGXT_FP_SLOTS][PGXT_PATH_MAX];

static int path_ends_with_png(const char *path) {
    size_t n = strlen(path);
    if (n < 4) return 0;
    const char *ext = path + n - 4;
    return (strcasecmp(ext, ".png") == 0);
}

void pgxt_register_fopen_path(FILE *fp, const char *path) {
    if (!fp || !path) return;
    if (!path_ends_with_png(path)) return;
    for (int i = 0; i < PGXT_FP_SLOTS; ++i) {
        if (g_fp[i] == NULL) {
            g_fp[i] = fp;
            strncpy(g_fp_path[i], path, PGXT_PATH_MAX - 1);
            g_fp_path[i][PGXT_PATH_MAX - 1] = '\0';
            return;
        }
    }
}

void pgxt_unregister_fopen(FILE *fp) {
    if (!fp) return;
    for (int i = 0; i < PGXT_FP_SLOTS; ++i) {
        if (g_fp[i] == fp) {
            g_fp[i] = NULL;
            g_fp_path[i][0] = '\0';
            return;
        }
    }
}

static const char *pgxt_lookup_fp_path(FILE *fp) {
    for (int i = 0; i < PGXT_FP_SLOTS; ++i) {
        if (g_fp[i] == fp) return g_fp_path[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * buffer → path registry (populated by io.c on fread of .png FILE*s) *
 *                                                                    *
 * Ring of 32 entries. The game fread's a .png into a malloc'd buffer *
 * and later calls cImage::LoadFromMem(buf, size). At that point      *
 * FILE* is already closed, so we have to key off the buffer address. *
 * ------------------------------------------------------------------ */

#define PGXT_BUF_SLOTS  32

typedef struct {
    void  *buf;
    size_t size;
    char   path[PGXT_PATH_MAX];
} pgxt_buf_slot_t;

static pgxt_buf_slot_t g_buf[PGXT_BUF_SLOTS];
static int             g_buf_next;  /* ring cursor for eviction */

void pgxt_register_fread(FILE *fp, void *buf, size_t size) {
    if (!fp || !buf || size == 0) return;
    const char *path = pgxt_lookup_fp_path(fp);
    if (!path) return;  /* not a .png FILE* */

    /* If the buffer is already registered, update (handles reused buffers). */
    for (int i = 0; i < PGXT_BUF_SLOTS; ++i) {
        if (g_buf[i].buf == buf) {
            g_buf[i].size = size;
            strncpy(g_buf[i].path, path, PGXT_PATH_MAX - 1);
            g_buf[i].path[PGXT_PATH_MAX - 1] = '\0';
            return;
        }
    }
    /* Otherwise insert at ring cursor. */
    int i = g_buf_next;
    g_buf_next = (g_buf_next + 1) % PGXT_BUF_SLOTS;
    g_buf[i].buf = buf;
    g_buf[i].size = size;
    strncpy(g_buf[i].path, path, PGXT_PATH_MAX - 1);
    g_buf[i].path[PGXT_PATH_MAX - 1] = '\0';
}

static const char *pgxt_lookup_buf_path(const void *buf, size_t size) {
    for (int i = 0; i < PGXT_BUF_SLOTS; ++i) {
        if (g_buf[i].buf == buf && g_buf[i].size == size) return g_buf[i].path;
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Per-decode state                                                   *
 *                                                                    *
 * Set by LoadFromMem hook, consumed by png_read_image hook. Decode   *
 * is single-threaded (preloader only does raw reads, not decode), so *
 * a single global slot is safe.                                      *
 * ------------------------------------------------------------------ */

static char g_current_png_path[PGXT_PATH_MAX];
static bool g_current_has_path;

/* ------------------------------------------------------------------ *
 * png_ptr → info_ptr registry (from png_read_info hook)              *
 * Used to validate rowbytes before memcpy'ing into the game's rows.  *
 * ------------------------------------------------------------------ */

#define PGXT_PNG_SLOTS  8

typedef struct {
    png_structp png;
    png_infop   info;
    bool        served;  /* set on successful sidecar short-circuit */
} pgxt_png_slot_t;

static pgxt_png_slot_t g_png[PGXT_PNG_SLOTS];

static pgxt_png_slot_t *pgxt_png_slot(png_structp png) {
    for (int i = 0; i < PGXT_PNG_SLOTS; ++i) {
        if (g_png[i].png == png) return &g_png[i];
    }
    return NULL;
}

static pgxt_png_slot_t *pgxt_png_slot_create(png_structp png) {
    for (int i = 0; i < PGXT_PNG_SLOTS; ++i) {
        if (g_png[i].png == NULL) {
            g_png[i].png = png;
            g_png[i].info = NULL;
            g_png[i].served = false;
            return &g_png[i];
        }
    }
    /* Evict slot 0, ring-shift. */
    for (int i = 0; i < PGXT_PNG_SLOTS - 1; ++i) g_png[i] = g_png[i + 1];
    g_png[PGXT_PNG_SLOTS - 1].png = png;
    g_png[PGXT_PNG_SLOTS - 1].info = NULL;
    g_png[PGXT_PNG_SLOTS - 1].served = false;
    return &g_png[PGXT_PNG_SLOTS - 1];
}

/* ------------------------------------------------------------------ *
 * Sidecar reader                                                     *
 * ------------------------------------------------------------------ */

#define PGX1_MAGIC_0 'P'
#define PGX1_MAGIC_1 'G'
#define PGX1_MAGIC_2 'X'
#define PGX1_MAGIC_3 '1'
#define PGX1_FMT_RGBA8 0u
#define PGX1_HEADER_SIZE 16

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static png_get_rowbytes_fn p_png_get_rowbytes;

static int pgxt_try_fill(png_structp png, png_bytepp rows) {
    if (!g_current_has_path) return 0;
    const char *path = g_current_png_path;

    char gxt_path[PGXT_PATH_MAX + 8];
    int np = snprintf(gxt_path, sizeof(gxt_path), "%s.gxt", path);
    if (np <= 0 || (size_t)np >= sizeof(gxt_path)) return 0;

    FILE *gxt = sceLibcBridge_fopen(gxt_path, "rb");
    if (!gxt) return 0;  /* no sidecar for this texture — normal */

    uint8_t header[PGX1_HEADER_SIZE];
    if (sceLibcBridge_fread(header, 1, PGX1_HEADER_SIZE, gxt) != PGX1_HEADER_SIZE) {
        sceLibcBridge_fclose(gxt);
        return 0;
    }
    if (header[0] != PGX1_MAGIC_0 || header[1] != PGX1_MAGIC_1 ||
        header[2] != PGX1_MAGIC_2 || header[3] != PGX1_MAGIC_3) {
        l_warn("[PGXT] %s: bad magic, ignoring sidecar", gxt_path);
        sceLibcBridge_fclose(gxt);
        return 0;
    }
    uint32_t sw  = rd_u32_le(header + 4);
    uint32_t sh  = rd_u32_le(header + 8);
    uint32_t fmt = rd_u32_le(header + 12);
    if (fmt != PGX1_FMT_RGBA8) {
        l_warn("[PGXT] %s: unsupported format %u", gxt_path, fmt);
        sceLibcBridge_fclose(gxt);
        return 0;
    }
    if (sw == 0 || sh == 0 || sw > 8192 || sh > 8192) {
        l_warn("[PGXT] %s: implausible dims %ux%u", gxt_path, sw, sh);
        sceLibcBridge_fclose(gxt);
        return 0;
    }

    size_t row_bytes = (size_t)sw * 4u;

    pgxt_png_slot_t *slot = pgxt_png_slot(png);
    if (slot && slot->info && p_png_get_rowbytes) {
        size_t expect = p_png_get_rowbytes(png, slot->info);
        if (expect != row_bytes) {
            l_warn("[PGXT] %s: rowbytes mismatch png=%u gxt=%u; skipping",
                   gxt_path, (unsigned)expect, (unsigned)row_bytes);
            sceLibcBridge_fclose(gxt);
            return 0;
        }
    }

    for (uint32_t y = 0; y < sh; ++y) {
        if (sceLibcBridge_fread(rows[y], 1, row_bytes, gxt) != row_bytes) {
            l_warn("[PGXT] %s: short read at row %u/%u", gxt_path, y, sh);
            sceLibcBridge_fclose(gxt);
            return 0;
        }
    }
    sceLibcBridge_fclose(gxt);

    if (!slot) slot = pgxt_png_slot_create(png);
    if (slot) slot->served = true;

    l_info("[PGXT] served %s (%ux%u, %u KB)",
           path, sw, sh, (unsigned)((row_bytes * sh) / 1024));
    return 1;
}

/* ------------------------------------------------------------------ *
 * Hooks                                                              *
 * ------------------------------------------------------------------ */

static so_hook g_hook_loadfrommem;
static so_hook g_hook_png_read_info;
static so_hook g_hook_png_read_image;
static so_hook g_hook_png_read_end;

/* int sys::cImage::LoadFromMem(cImage *this, const void *buf, uint32_t size) */
static int loadfrommem_hook(void *self, const void *buf, uint32_t size) {
    l_info("[PGXT] lfm enter self=%p buf=%p size=%u", self, buf, (unsigned)size);
    const char *path = pgxt_lookup_buf_path(buf, size);
    if (path) {
        strncpy(g_current_png_path, path, PGXT_PATH_MAX - 1);
        g_current_png_path[PGXT_PATH_MAX - 1] = '\0';
        g_current_has_path = true;
    } else {
        /* Diagnostic: no buf→path match. Tells us whether the game chunked
         * the fread into sub-buffers or used a different load path. Paired
         * with the [PGXT] served line, these two logs disambiguate "never
         * hit LoadFromMem" vs "hit it but missed the registry". */
        l_info("[PGXT] LoadFromMem miss buf=%p size=%u", buf, (unsigned)size);
    }
    int r = SO_CONTINUE(int, g_hook_loadfrommem, self, buf, size);
    g_current_has_path = false;
    g_current_png_path[0] = '\0';
    return r;
}

static void png_read_info_hook(png_structp png, png_infop info) {
    pgxt_png_slot_t *slot = pgxt_png_slot(png);
    if (!slot) slot = pgxt_png_slot_create(png);
    if (slot) slot->info = info;

    so_hook_unpatch(&g_hook_png_read_info);
    if (g_hook_png_read_info.thumb_addr) {
        ((void (*)(png_structp, png_infop))g_hook_png_read_info.thumb_addr)(png, info);
    } else {
        ((void (*)(png_structp, png_infop))g_hook_png_read_info.addr)(png, info);
    }
    so_hook_repatch(&g_hook_png_read_info);
}

static void png_read_image_hook(png_structp png, png_bytepp rows) {
    l_info("[PGXT] read_image enter png=%p rows=%p has_path=%d",
           png, rows, (int)g_current_has_path);
    if (pgxt_try_fill(png, rows)) {
        return;
    }
    so_hook_unpatch(&g_hook_png_read_image);
    if (g_hook_png_read_image.thumb_addr) {
        ((void (*)(png_structp, png_bytepp))g_hook_png_read_image.thumb_addr)(png, rows);
    } else {
        ((void (*)(png_structp, png_bytepp))g_hook_png_read_image.addr)(png, rows);
    }
    so_hook_repatch(&g_hook_png_read_image);
}

static void png_read_end_hook(png_structp png, png_infop info) {
    pgxt_png_slot_t *slot = pgxt_png_slot(png);
    if (slot && slot->served) {
        /* We skipped png_read_image, so the IDAT chunk stream is unconsumed.
         * Running the original png_read_end on this half-parsed state would
         * crash. Clear the slot and silently succeed — png_destroy_read_struct
         * will run later and free libpng's internal buffers. */
        slot->png = NULL;
        slot->info = NULL;
        slot->served = false;
        return;
    }

    so_hook_unpatch(&g_hook_png_read_end);
    if (g_hook_png_read_end.thumb_addr) {
        ((void (*)(png_structp, png_infop))g_hook_png_read_end.thumb_addr)(png, info);
    } else {
        ((void (*)(png_structp, png_infop))g_hook_png_read_end.addr)(png, info);
    }
    so_hook_repatch(&g_hook_png_read_end);

    /* Clear slot now that the PNG is done. */
    if (slot) {
        slot->png = NULL;
        slot->info = NULL;
        slot->served = false;
    }
}

void pgxt_install_hooks(void) {
    p_png_get_rowbytes = (png_get_rowbytes_fn)so_symbol(&so_mod, "png_get_rowbytes");

    uintptr_t lfm_addr   = so_symbol(&so_mod, "_ZN3sys6cImage11LoadFromMemEPKvj");
    uintptr_t info_addr  = so_symbol(&so_mod, "png_read_info");
    uintptr_t image_addr = so_symbol(&so_mod, "png_read_image");
    uintptr_t end_addr   = so_symbol(&so_mod, "png_read_end");

    if (!lfm_addr || !info_addr || !image_addr || !end_addr) {
        l_warn("[PGXT] required symbols missing "
               "(LoadFromMem=%p read_info=%p read_image=%p read_end=%p); "
               "hooks not installed",
               (void *)lfm_addr, (void *)info_addr,
               (void *)image_addr, (void *)end_addr);
        return;
    }

    g_hook_loadfrommem    = hook_addr(lfm_addr,   (uintptr_t)&loadfrommem_hook);
    g_hook_png_read_info  = hook_addr(info_addr,  (uintptr_t)&png_read_info_hook);
    g_hook_png_read_image = hook_addr(image_addr, (uintptr_t)&png_read_image_hook);
    g_hook_png_read_end   = hook_addr(end_addr,   (uintptr_t)&png_read_end_hook);
    l_info("[PGXT] hooks installed (LoadFromMem=0x%x read_info=0x%x "
           "read_image=0x%x read_end=0x%x)",
           (unsigned)lfm_addr, (unsigned)info_addr,
           (unsigned)image_addr, (unsigned)end_addr);
}
