#include "utils/game_music_io.h"
#include "reimpl/io.h"
#include "utils/logger.h"

#include <so_util/so_util.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern so_module fmod_mod;

/* Result values and callback signatures from the supplied FMOD library. */
enum { IO_OK = 0, IO_BAD = 13, IO_EOF = 16, IO_NOT_FOUND = 18,
       IO_INTERNAL = 28, IO_INVALID_PARAM = 31, IO_NO_MEMORY = 38 };

typedef struct {
    FILE *stream;
    unsigned int position;
    unsigned int size;
    char path[];
} music_file;

static int music_file_open(const char *name, unsigned int *size,
                           void **handle, void *userdata) {
    (void)userdata;
    if (!name || !size || !handle) return IO_INVALID_PARAM;
    *handle = NULL;
    *size = 0;
    size_t path_length = strnlen(name, 512);
    if (path_length == 512) return IO_INVALID_PARAM;

    FILE *stream = fopen_soloader(name, "rb");
    if (!stream) return IO_NOT_FOUND;
    long length = -1;
    if (fseek_soloader(stream, 0, SEEK_END) == 0)
        length = ftell_soloader(stream);
    if (length < 0 || length > INT32_MAX ||
        fseek_soloader(stream, 0, SEEK_SET) != 0) {
        fclose_soloader(stream);
        return IO_BAD;
    }

    music_file *file = malloc(sizeof(*file) + path_length + 1);
    if (!file) {
        fclose_soloader(stream);
        return IO_NO_MEMORY;
    }
    file->stream = stream;
    file->position = 0;
    file->size = (unsigned int)length;
    memcpy(file->path, name, path_length + 1);
    *size = file->size;
    *handle = file;
    return IO_OK;
}

static int music_file_close(void *handle, void *userdata) {
    (void)userdata;
    if (!handle) return IO_INVALID_PARAM;
    music_file *file = handle;
    int result = file->stream ? fclose_soloader(file->stream) : 0;
    free(file);
    return result == 0 ? IO_OK : IO_BAD;
}

/* FMOD keeps this wrapper while its I/O thread replaces the invalid FILE.
 * Preserve the logical offset, independently of stdio's read-ahead buffer. */
static bool reopen_at(music_file *file, unsigned int position) {
    if (file->stream) fclose_soloader(file->stream);
    file->stream = fopen_soloader(file->path, "rb");
    if (!file->stream) return false;
    if (fseek_soloader(file->stream, (long)position, SEEK_SET) != 0) {
        fclose_soloader(file->stream);
        file->stream = NULL;
        return false;
    }
    file->position = position;
    l_audio("[MUSIC-IO] reopened offset=%u path=%s", position, file->path);
    return true;
}

static int music_file_read(void *handle, void *buffer, unsigned int requested,
                           unsigned int *bytes_read, void *userdata) {
    (void)userdata;
    if (!handle || !bytes_read || (!buffer && requested)) return IO_INVALID_PARAM;
    music_file *file = handle;
    *bytes_read = 0;
    if (!requested) return IO_OK;

    /* The assets are read-only and their length was supplied to FMOD at open.
     * A short read before that boundary is a failure, not ordinary EOF. */
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt && !reopen_at(file, file->position)) break;
        if (file->stream) {
            unsigned int got = (unsigned int)fread_soloader(
                    (char *)buffer + *bytes_read, 1,
                    requested - *bytes_read, file->stream);
            file->position += got;
            *bytes_read += got;
        }
        if (*bytes_read == requested) return IO_OK;
        if (file->position >= file->size) return IO_EOF;
    }
    l_audio("[MUSIC-IO] read failed offset=%u got=%u requested=%u path=%s",
            file->position, *bytes_read, requested, file->path);
    return IO_BAD;
}

static int music_file_seek(void *handle, unsigned int position, void *userdata) {
    (void)userdata;
    if (!handle) return IO_INVALID_PARAM;
    music_file *file = handle;
    if (position > INT32_MAX) return IO_BAD;
    if (file->stream && fseek_soloader(file->stream, (long)position, SEEK_SET) == 0) {
        file->position = position;
        return IO_OK;
    }
    return reopen_at(file, position) ? IO_OK : IO_BAD;
}

int game_music_io_install(void *system) {
    typedef int (*set_file_system_fn)(void *,
            int (*)(const char *, unsigned int *, void **, void *),
            int (*)(void *, void *),
            int (*)(void *, void *, unsigned int, unsigned int *, void *),
            int (*)(void *, unsigned int, void *),
            int (*)(void *, void *), int (*)(void *, void *), int);
    set_file_system_fn set_file_system = (set_file_system_fn)so_symbol(&fmod_mod,
            "_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKcPjPPvS5_EPFS1_S5_S5_"
            "EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESI_i");
    if (!set_file_system) return IO_INTERNAL;
    /* Keep FMOD's existing streaming thread and buffer alignment. */
    return set_file_system(system, music_file_open, music_file_close,
            music_file_read, music_file_seek, NULL, NULL, -1);
}
