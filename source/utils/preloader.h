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

/* Read-only memory stream, tracked like a cache hit. The buffer is borrowed
 * and must remain valid until fclose; closing never frees or modifies it. */
FILE *preloader_open_memory(const void *buf, size_t size);

/* Non-zero if fp was returned by a preloader open/adopt function. Route
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
 * Animation files still perform many small reads after bulk decoding;
 * these helpers avoid stdio locking and buffering on those reads. */
int    preloader_is_slurp(FILE *fp);
/* Classify and read in one lookup. Return 0 for an untracked stream, 1
 * for a newlib cache handle, or 2 for an owned slurp handle. Only the
 * latter is restricted to animation/text files and can skip PNG tracking. */
int preloader_read(FILE *fp, void *dst, size_t size, size_t count, size_t *result);
/* Take a complete record without advancing on failure. */
const unsigned char *preloader_slurp_take(FILE *fp, size_t nbytes);
size_t preloader_slurp_fast_read(FILE *fp, void *dst, size_t nbytes);
int    preloader_slurp_fast_seek(FILE *fp, long offset, int whence);
long   preloader_slurp_fast_tell(FILE *fp);

#endif
