/*
 * pgxt.c — runtime support for PGX1 (pre-decoded RGBA8) texture sidecars.
 *
 * The website packager emits a foo.png.gxt sidecar alongside each hot PNG.
 * The game loads PNGs by fopen + fread into a memory buffer, then passes
 * that buffer to sys::cImage::LoadFromMem, which internally uses libpng's
 * memory-reader API (png_set_read_fn — NOT png_init_io). So FILE* is gone
 * by the time libpng runs. We correlate paths to LoadFromMem buffers via:
 *
 *   fopen(path)           → io.c records FILE*→path
 *   fread(buf, size, fp)  → pgxt records buf→path (for .png FILE*s)
 *   fclose(fp)            → io.c clears FILE*→path
 *   LoadFromMem(this,buf) → pgxt looks up buf→path, sets current_png_path
 *   png_read_image(...)   → pgxt serves foo.png.gxt if current_png_path set
 *   png_read_end(...)     → pgxt no-ops when a sidecar was served
 *
 * Sidecar layout (little-endian):
 *   off  0  u8[4]  magic   "PGX1"
 *   off  4  u32    width
 *   off  8  u32    height
 *   off 12  u32    format  (0 = RGBA8 row-major, top-left origin)
 *   off 16  u8[]   pixels  (width * height * 4 bytes)
 */
#ifndef PACMAN_VITA_PGXT_H
#define PACMAN_VITA_PGXT_H

#include <stddef.h>
#include <stdio.h>

/* io.c calls these on fopen / fclose / fread. Only .png paths are recorded;
 * other paths are a no-op. */
void pgxt_register_fopen_path(FILE *fp, const char *path);
void pgxt_unregister_fopen(FILE *fp);
void pgxt_register_fread(FILE *fp, void *buf, size_t size);

/* Install the cImage::LoadFromMem / png_read_info / png_read_image /
 * png_read_end hooks. Call once after so_mod is loaded and symbols are
 * resolvable. Safe to call before the game opens any PNG. */
void pgxt_install_hooks(void);

#endif
