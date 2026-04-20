/*
 * Background file preloader.
 *
 * Spawns a thread at startup that reads a warmup list of slow-to-load
 * files into an in-memory cache. The io.c fopen shim calls
 * preloader_try_open first; on a cache hit we return a real FILE* backed
 * by the cached buffer (via fmemopen), so the game's native fread/fseek/
 * fclose — including libraries like FreeType that bypass our shims and
 * dereference the FILE* as a newlib __sFILE — Just Work.
 */
#ifndef PACMAN_VITA_PRELOADER_H
#define PACMAN_VITA_PRELOADER_H

#include <stddef.h>
#include <stdio.h>

void preloader_start(void);

/* Returns a real fmemopen'd FILE* on cache hit, NULL on miss. */
FILE *preloader_try_open(const char *path);

/* Non-zero if fp was returned by preloader_try_open. Caller should route
 * fread/fseek/ftell/fclose through newlib stdio (not sceLibcBridge_*)
 * because the underlying FILE* is a newlib __sFILE, not a sceLibc one. */
int preloader_is_preloaded(FILE *fp);

/* Release tracking for fp when the game closes it. Also frees any buffer
 * the preloader owns for this fp (from preloader_slurp_adopt). */
void preloader_fclose_tracked(FILE *fp);

/* Wrap a caller-provided malloc'd buffer in an fmemopen'd FILE*, taking
 * ownership of the buffer. On success, the returned FILE* is tracked so
 * io.c routes fread/fseek/fclose through newlib, and the buffer is freed
 * when the game closes the handle. On failure, the buffer is freed and
 * NULL is returned. Size must be > 0. */
FILE *preloader_slurp_adopt(void *buf, size_t size);

/* Fast-path stdio over slurp handles. Bypasses newlib entirely — pure
 * memcpy + an internal position counter. Callers must gate on
 * preloader_is_slurp(fp) first; if the handle isn't a slurp, these return
 * a don't-care value (0 / -1) and the caller should fall back.
 *
 * Why: fmemopen'd newlib fread has ~1 µs/call lock+buffer overhead. At
 * 233k tight fread calls on menu.runnybin, that's ~230 ms floor — so the
 * fast path is exactly the difference between a 100 ms slurp win and a
 * 300 ms slurp win. */
int    preloader_is_slurp(FILE *fp);
size_t preloader_slurp_fast_read(FILE *fp, void *dst, size_t nbytes);
int    preloader_slurp_fast_seek(FILE *fp, long offset, int whence);
long   preloader_slurp_fast_tell(FILE *fp);

#endif
