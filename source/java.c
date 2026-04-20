/*
 * java.c
 *
 * FalsoJNI implementation for PAC-MAN Championship Edition DX.
 *
 * Implements the Java-side callbacks that libPacmanCE.so calls back into
 * via JNI: APKFileHelper (asset reading), FileHelper (save data I/O),
 * and RateMeManager (stubbed).
 *
 * Copyright (C) 2026 Ellie J Turner
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>
#include <string.h>
#include <stdio.h>
#include "utils/settings.h"
#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>
#include <vitasdk.h>

#include "utils/logger.h"

/*
 * APKFileHelper reimplementation
 *
 * The native code reads game assets (textures, sounds, levels) via JNI
 * callbacks to APKFileHelper. On Android, this reads from the APK's
 * assets/ directory. On Vita, we read from ux0:data/pacmancedx/assets/.
 *
 * Flow: getInstance() -> openFileAndroid(name) -> read length field ->
 *       readFileAndroid(file, n) -> read data field -> closeFileAndroid(file)
 *
 * APKFile fields (from smali):
 *   length (int)   - total file size
 *   position (int) - current read position
 *   data (byte[])  - read buffer (JavaDynArray*)
 *   bufferSize (int) - current buffer allocation size
 */

/* Our APKFile structure - returned as an opaque jobject */
typedef struct {
    FILE *fp;
    int length;
    int position;
    int bufferSize;
    JavaDynArray *data;  /* byte[] buffer for reads */
} VitaAPKFile;

/* Global state for field access (FalsoJNI fields are global, not per-object) */
static VitaAPKFile *g_current_apk_file = NULL;

/* ===== Load-time profiling counters =====
 * These aggregate file I/O statistics so we can figure out what
 * actually dominates startup time (PNGs, data files, etc.). */
uint64_t g_prof_open_count   = 0;
uint64_t g_prof_open_us      = 0;  /* time spent in fopen */
uint64_t g_prof_read_count   = 0;
uint64_t g_prof_read_bytes   = 0;
uint64_t g_prof_read_us      = 0;  /* time spent in fread */
/* Per-extension breakdown */
uint64_t g_prof_png_bytes    = 0;
uint64_t g_prof_png_us       = 0;
uint64_t g_prof_ogg_bytes    = 0;
uint64_t g_prof_ogg_us       = 0;
uint64_t g_prof_other_bytes  = 0;
uint64_t g_prof_other_us     = 0;

static inline uint64_t prof_now_us(void) {
    return sceKernelGetProcessTimeWide();
}

static int prof_is_ext(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return 0;
    for (size_t i = 0; i < elen; ++i) {
        char a = name[nlen - elen + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Category tracked per open file for read accounting */
static int g_prof_current_cat = 0; /* 0=other, 1=png, 2=ogg */

/*
 * FileHelper reimplementation
 *
 * Used for save data: openInputFile/openOutputFile/readFile/writeFile.
 * On Vita, files go to ux0:data/pacmancedx/files/
 */
typedef struct {
    FILE *fp;
} VitaFileStream;

static JavaDynArray *g_file_helper_buffer = NULL;

/* Android SDK version */
const int SDK_INT = 14;

/*
 * Method ID assignments. FalsoJNI maps method names -> numeric IDs,
 * then dispatches by ID and return type.
 */
enum {
    /* APKFileHelper */
    MID_APK_GET_INSTANCE       = 1,
    MID_APK_OPEN_FILE          = 2,
    MID_APK_CLOSE_FILE         = 3,
    MID_APK_READ_FILE          = 4,
    MID_APK_SEEK_FILE          = 5,

    /* FileHelper */
    MID_FH_GET_INSTANCE        = 6,
    MID_FH_OPEN_INPUT          = 7,
    MID_FH_OPEN_OUTPUT         = 8,
    MID_FH_READ_FILE           = 9,
    MID_FH_WRITE_FILE          = 10,
    MID_FH_CLOSE_INPUT         = 11,
    MID_FH_CLOSE_OUTPUT        = 12,
    MID_FH_WRITE_FILE_SD       = 13,
    MID_FH_GET_LANGUAGE_ID     = 14,
    MID_FH_GET_DATE_YEAR       = 15,
    MID_FH_GET_DATE_MONTH      = 16,
    MID_FH_GET_DATE_DAY        = 17,
    MID_FH_IS_TRIAL            = 18,
    MID_FH_ACCESS_EVERY_MISSION = 19,

    /* RateMeManager */
    MID_RATE_SET_MSG           = 20,

    /* Misc */
    MID_GET_INSTALL_LOCATION   = 21,
    MID_SET_RATE_APP_TEXTS     = 22,

    /* Additional methods the game calls */
    MID_HAS_LOW_PERFORMANCE    = 23,
    MID_GET_LANGUAGE           = 24,
    MID_KEEP_SCREEN_ON         = 25,
    MID_GET_BUILD_VERSION      = 26,

    /* FMOD Java bridge */
    MID_FMOD_CHECK_INIT        = 27,
    MID_FMOD_SUPPORTS_LL       = 28,
    MID_FMOD_SUPPORTS_AAUDIO   = 29,
    MID_FMOD_GET_OUT_BLOCK     = 30,
    MID_FMOD_PLAYBACK_STREAM   = 31,
    MID_FMOD_RECORDING_PRESET  = 32,
    MID_FMOD_AUDIO_INIT        = 33,
    MID_FMOD_AUDIO_CLOSE       = 34,
    MID_FMOD_AUDIO_WRITE       = 35,
    MID_FMOD_AUDIO_CTOR        = 36,
    MID_FMOD_GET_OUT_SR        = 37,
};

/* Field ID assignments */
enum {
    FID_LENGTH       = 1,
    FID_POSITION     = 2,
    FID_DATA         = 3,
    FID_BUFFER_SIZE  = 4,
    FID_BUFFER       = 5,   /* FileHelper.Buffer */
    FID_SDK_INT      = 6,
    FID_WINDOW_SERVICE = 7,
    FID_S_LANGUAGE_ID = 8,
};

NameToMethodID nameToMethodId[] = {
    /* APKFileHelper */
    { MID_APK_GET_INSTANCE,    "getInstance",       METHOD_TYPE_OBJECT },
    { MID_APK_OPEN_FILE,       "openFileAndroid",   METHOD_TYPE_OBJECT },
    { MID_APK_CLOSE_FILE,      "closeFileAndroid",  METHOD_TYPE_VOID },
    { MID_APK_READ_FILE,       "readFileAndroid",   METHOD_TYPE_VOID },
    { MID_APK_SEEK_FILE,       "seekFileAndroid",   METHOD_TYPE_LONG },

    /* FileHelper (getInstance is shared with APKFileHelper above) */
    { MID_FH_OPEN_INPUT,       "openInputFile",     METHOD_TYPE_OBJECT },
    { MID_FH_OPEN_OUTPUT,      "openOutputFile",    METHOD_TYPE_OBJECT },
    { MID_FH_READ_FILE,        "readFile",          METHOD_TYPE_VOID },
    { MID_FH_WRITE_FILE,       "writeFile",         METHOD_TYPE_VOID },
    { MID_FH_CLOSE_INPUT,      "closeInputFile",    METHOD_TYPE_VOID },
    { MID_FH_CLOSE_OUTPUT,     "closeOutputFile",   METHOD_TYPE_VOID },
    { MID_FH_WRITE_FILE_SD,    "writeFileToSD",     METHOD_TYPE_VOID },
    { MID_FH_GET_LANGUAGE_ID,  "GetLanguageID",     METHOD_TYPE_INT },
    { MID_FH_GET_DATE_YEAR,    "GetDate_Year",      METHOD_TYPE_INT },
    { MID_FH_GET_DATE_MONTH,   "GetDate_Month",     METHOD_TYPE_INT },
    { MID_FH_GET_DATE_DAY,     "GetDate_Day",       METHOD_TYPE_INT },
    { MID_FH_IS_TRIAL,         "IsTrial",           METHOD_TYPE_BOOLEAN },
    { MID_FH_ACCESS_EVERY_MISSION, "AccessEveryMission", METHOD_TYPE_BOOLEAN },

    /* RateMeManager */
    { MID_RATE_SET_MSG,        "SetRateMsg",        METHOD_TYPE_VOID },

    /* Misc */
    { MID_GET_INSTALL_LOCATION, "GetInstallLocation", METHOD_TYPE_OBJECT },
    { MID_SET_RATE_APP_TEXTS,  "SetRateAppTexts",   METHOD_TYPE_VOID },

    /* Additional methods */
    { MID_HAS_LOW_PERFORMANCE, "HasLowPerformance", METHOD_TYPE_BOOLEAN },
    { MID_GET_LANGUAGE,        "GetLanguage",       METHOD_TYPE_OBJECT },
    { MID_KEEP_SCREEN_ON,      "KeepScreenOn",      METHOD_TYPE_VOID },
    { MID_GET_BUILD_VERSION,   "GetBuildVersion",   METHOD_TYPE_OBJECT },

    /* FMOD Java methods used by libfmod */
    { MID_FMOD_CHECK_INIT,      "checkInit",                         METHOD_TYPE_BOOLEAN },
    { MID_FMOD_SUPPORTS_LL,     "supportsLowLatency",                METHOD_TYPE_BOOLEAN },
    { MID_FMOD_SUPPORTS_AAUDIO, "supportsAAudio",                    METHOD_TYPE_BOOLEAN },
    { MID_FMOD_GET_OUT_BLOCK,   "getOutputBlockSize",                METHOD_TYPE_INT },
    { MID_FMOD_GET_OUT_SR,      "getOutputSampleRate",               METHOD_TYPE_INT },
    { MID_FMOD_PLAYBACK_STREAM, "androidPlaybackStreamType",         METHOD_TYPE_INT },
    { MID_FMOD_RECORDING_PRESET,"androidRecordingPreset",            METHOD_TYPE_INT },
    { MID_FMOD_AUDIO_INIT,      "init",                              METHOD_TYPE_BOOLEAN },
    { MID_FMOD_AUDIO_CLOSE,     "close",                             METHOD_TYPE_VOID },
    { MID_FMOD_AUDIO_WRITE,     "write",                             METHOD_TYPE_VOID },
    { MID_FMOD_AUDIO_CTOR,      "org/fmod/AudioDevice/<init>",       METHOD_TYPE_OBJECT },
};

/* --- APKFileHelper method implementations --- */

/*
 * getInstance() -> returns APKFileHelper singleton (opaque pointer)
 * Both APKFileHelper and FileHelper have getInstance(). FalsoJNI resolves
 * by name only, so the first match wins. Since both just need to return
 * a non-null pointer, this is fine.
 */
static jobject apk_getInstance(jmethodID id, va_list args) {
    static int singleton = 1;
    fjni_log_dbg("APKFileHelper.getInstance()");
    return (jobject)&singleton;
}

/*
 * openFileAndroid(String filename) -> APKFile
 * Opens a file from DATA_PATH/assets/ (or DATA_PATH directly for some paths)
 */
static jobject apk_openFileAndroid(jmethodID id, va_list args) {
    /* The filename comes as a jstring (JavaString*) from JNI, not a raw C string */
    jstring filenameStr = va_arg(args, jstring);
    if (!filenameStr) {
        l_error("openFileAndroid: NULL filename");
        return NULL;
    }

    const char *filename = (*(&jni))->GetStringUTFChars(&jni, filenameStr, NULL);
    if (!filename) {
        l_error("openFileAndroid: GetStringUTFChars failed");
        return NULL;
    }

    /* The game requests assets by name like "sound/foo.ogg", "data/bar.bin" etc.
     * On Android these come from the APK assets/ folder.
     * On Vita we look in DATA_PATH/assets/ */
    char path[512];
    snprintf(path, sizeof(path), "%sassets/%s", DATA_PATH, filename);

    uint64_t _prof_t0 = prof_now_us();
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        /* Try without assets/ prefix */
        snprintf(path, sizeof(path), "%s%s", DATA_PATH, filename);
        fp = fopen(path, "rb");
    }
    g_prof_open_us += prof_now_us() - _prof_t0;
    g_prof_open_count++;
    /* Classify the file by extension for the breakdown */
    if (prof_is_ext(filename, ".png")) g_prof_current_cat = 1;
    else if (prof_is_ext(filename, ".ogg") || prof_is_ext(filename, ".mp3")) g_prof_current_cat = 2;
    else g_prof_current_cat = 0;
    if (!fp) {
        l_error("openFileAndroid: cannot open '%s'", filename);
        (*(&jni))->ReleaseStringUTFChars(&jni, filenameStr, filename);
        return NULL;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    int length = (int)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    VitaAPKFile *apk = (VitaAPKFile *)malloc(sizeof(VitaAPKFile));
    apk->fp = fp;
    apk->length = length;
    apk->position = 0;
    /* Pre-allocate buffer to the file size (capped at 4MB) to avoid
     * repeated realloc cycles when reading large audio assets. */
    int init_buf = length < (4 * 1024 * 1024) ? length : (4 * 1024 * 1024);
    if (init_buf < 1024) init_buf = 1024;
    apk->bufferSize = init_buf;
    apk->data = jda_alloc(init_buf, FIELD_TYPE_BYTE);

    /* Update global state and FalsoJNI fields immediately */
    g_current_apk_file = apk;
    setIntFieldValueById((jfieldID)FID_LENGTH, apk->length);
    setIntFieldValueById((jfieldID)FID_POSITION, apk->position);
    setIntFieldValueById((jfieldID)FID_BUFFER_SIZE, apk->bufferSize);
    setObjectFieldValueById((jfieldID)FID_DATA, (jobject)apk->data);

    l_info("openFileAndroid('%s'): size=%d", filename, length);
    (*(&jni))->ReleaseStringUTFChars(&jni, filenameStr, filename);
    return (jobject)apk;
}

/*
 * closeFileAndroid(APKFile file)
 */
static void apk_closeFileAndroid(jmethodID id, va_list args) {
    VitaAPKFile *apk = va_arg(args, VitaAPKFile *);
    if (!apk) return;

    fjni_log_dbg("closeFileAndroid");
    if (apk->fp) fclose(apk->fp);
    if (apk->data) jda_free(apk->data);
    free(apk);

    if (g_current_apk_file == apk)
        g_current_apk_file = NULL;
}

/*
 * readFileAndroid(APKFile file, int numBytes)
 * Reads numBytes from the file into the APKFile's data buffer.
 * The native code then accesses data via GetObjectField + GetByteArrayElements.
 */
static void apk_readFileAndroid(jmethodID id, va_list args) {
    VitaAPKFile *apk = va_arg(args, VitaAPKFile *);
    int numBytes = va_arg(args, int);

    if (!apk || !apk->fp) {
        l_error("readFileAndroid: invalid APKFile");
        return;
    }

    /* Grow buffer if needed (matches Java behavior) */
    if (numBytes > apk->bufferSize) {
        if (apk->data) jda_free(apk->data);
        apk->data = jda_alloc(numBytes, FIELD_TYPE_BYTE);
        apk->bufferSize = numBytes;
    }

    uint64_t _prof_rt0 = prof_now_us();
    int bytesRead = (int)fread(apk->data->array, 1, numBytes, apk->fp);
    uint64_t _prof_rdt = prof_now_us() - _prof_rt0;
    apk->position += bytesRead;
    g_prof_read_count++;
    g_prof_read_bytes += (uint64_t)bytesRead;
    g_prof_read_us    += _prof_rdt;
    if (g_prof_current_cat == 1) {
        g_prof_png_bytes += (uint64_t)bytesRead;
        g_prof_png_us    += _prof_rdt;
    } else if (g_prof_current_cat == 2) {
        g_prof_ogg_bytes += (uint64_t)bytesRead;
        g_prof_ogg_us    += _prof_rdt;
    } else {
        g_prof_other_bytes += (uint64_t)bytesRead;
        g_prof_other_us    += _prof_rdt;
    }

    /* Update FalsoJNI fields immediately so native can read data */
    g_current_apk_file = apk;
    setIntFieldValueById((jfieldID)FID_POSITION, apk->position);
    setIntFieldValueById((jfieldID)FID_BUFFER_SIZE, apk->bufferSize);
    setObjectFieldValueById((jfieldID)FID_DATA, (jobject)apk->data);
}

/*
 * seekFileAndroid(APKFile file, int offset) -> long
 * Resets to start and skips forward by offset bytes.
 */
static jlong apk_seekFileAndroid(jmethodID id, va_list args) {
    VitaAPKFile *apk = va_arg(args, VitaAPKFile *);
    int offset = va_arg(args, int);

    if (!apk || !apk->fp) return 0;

    fseek(apk->fp, offset, SEEK_SET);
    apk->position = offset;
    g_current_apk_file = apk;
    setIntFieldValueById((jfieldID)FID_POSITION, apk->position);

    return (jlong)offset;
}

/* --- FileHelper method implementations --- */

/*
 * openInputFile(String filename) -> FileInputStream
 * Opens a file from DATA_PATH/files/ for reading (save data).
 */
static jobject fh_openInputFile(jmethodID id, va_list args) {
    jstring filenameStr = va_arg(args, jstring);
    if (!filenameStr) return NULL;

    const char *filename = (*(&jni))->GetStringUTFChars(&jni, filenameStr, NULL);
    if (!filename) return NULL;

    char path[512];
    snprintf(path, sizeof(path), "%s%s", SAVE_PATH, filename);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        l_warn("openInputFile: cannot open '%s'", filename);
        (*(&jni))->ReleaseStringUTFChars(&jni, filenameStr, filename);
        return NULL;
    }

    VitaFileStream *stream = (VitaFileStream *)malloc(sizeof(VitaFileStream));
    stream->fp = fp;

    l_info("openInputFile('%s')", filename);
    (*(&jni))->ReleaseStringUTFChars(&jni, filenameStr, filename);
    return (jobject)stream;
}

/*
 * openOutputFile(String filename) -> FileOutputStream
 */
static jobject fh_openOutputFile(jmethodID id, va_list args) {
    jstring filenameStr = va_arg(args, jstring);
    if (!filenameStr) return NULL;

    const char *filename = (*(&jni))->GetStringUTFChars(&jni, filenameStr, NULL);
    if (!filename) return NULL;

    char path[512];
    snprintf(path, sizeof(path), "%s%s", SAVE_PATH, filename);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        l_error("openOutputFile: cannot open '%s'", filename);
        (*(&jni))->ReleaseStringUTFChars(&jni, filenameStr, filename);
        return NULL;
    }

    VitaFileStream *stream = (VitaFileStream *)malloc(sizeof(VitaFileStream));
    stream->fp = fp;

    l_info("openOutputFile('%s')", filename);
    (*(&jni))->ReleaseStringUTFChars(&jni, filenameStr, filename);
    return (jobject)stream;
}

/*
 * readFile(int numBytes, FileInputStream fis)
 * Reads data into FileHelper.Buffer field.
 */
static void fh_readFile(jmethodID id, va_list args) {
    int numBytes = va_arg(args, int);
    VitaFileStream *stream = va_arg(args, VitaFileStream *);

    if (!stream || !stream->fp) return;

    /* Allocate/reallocate the Buffer */
    if (g_file_helper_buffer) jda_free(g_file_helper_buffer);
    g_file_helper_buffer = jda_alloc(numBytes, FIELD_TYPE_BYTE);

    fread(g_file_helper_buffer->array, 1, numBytes, stream->fp);

    /* Update Buffer field so native code can access it */
    setObjectFieldValueById((jfieldID)FID_BUFFER, (jobject)g_file_helper_buffer);
}

/*
 * writeFile(byte[] data, int length, FileOutputStream fos)
 */
static void fh_writeFile(jmethodID id, va_list args) {
    JavaDynArray *data = va_arg(args, JavaDynArray *);
    int length = va_arg(args, int);
    VitaFileStream *stream = va_arg(args, VitaFileStream *);

    if (!stream || !stream->fp || !data) return;

    fwrite(data->array, 1, length, stream->fp);
}

static void fh_closeInputFile(jmethodID id, va_list args) {
    VitaFileStream *stream = va_arg(args, VitaFileStream *);
    if (stream) {
        if (stream->fp) fclose(stream->fp);
        free(stream);
    }
}

static void fh_closeOutputFile(jmethodID id, va_list args) {
    VitaFileStream *stream = va_arg(args, VitaFileStream *);
    if (stream) {
        if (stream->fp) fclose(stream->fp);
        free(stream);
    }
}

static void fh_writeFileToSD(jmethodID id, va_list args) {
    /* No SD card on Vita - stub */
    fjni_log_dbg("writeFileToSD: stubbed");
}

static jint fh_GetLanguageID(jmethodID id, va_list args) {
    int lang = -1;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    /* Map Vita language IDs to game's expected IDs.
     * The game uses -1 for auto-detect, which should work fine. */
    switch (lang) {
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:  return 0;
        case SCE_SYSTEM_PARAM_LANG_ENGLISH_US:
        case SCE_SYSTEM_PARAM_LANG_ENGLISH_GB: return 1;
        case SCE_SYSTEM_PARAM_LANG_FRENCH:    return 2;
        case SCE_SYSTEM_PARAM_LANG_SPANISH:   return 3;
        case SCE_SYSTEM_PARAM_LANG_GERMAN:    return 4;
        case SCE_SYSTEM_PARAM_LANG_ITALIAN:   return 5;
        case SCE_SYSTEM_PARAM_LANG_KOREAN:    return 6;
        case SCE_SYSTEM_PARAM_LANG_CHINESE_T: return 7;
        case SCE_SYSTEM_PARAM_LANG_CHINESE_S: return 8;
        default: return 1; /* English */
    }
}

static jint fh_GetDate_Year(jmethodID id, va_list args)  { return 2026; }
static jint fh_GetDate_Month(jmethodID id, va_list args) { return 1; }
static jint fh_GetDate_Day(jmethodID id, va_list args)   { return 1; }

static jboolean fh_IsTrial(jmethodID id, va_list args) {
    return JNI_FALSE; /* Full version */
}

static jboolean fh_AccessEveryMission(jmethodID id, va_list args) {
    return setting_accessAllMissions ? JNI_TRUE : JNI_FALSE;
}

/* --- RateMeManager --- */

static void rate_SetRateMsg(jmethodID id, va_list args) {
    /* No-op on Vita */
    fjni_log_dbg("SetRateMsg: stubbed");
}

static void dummy_void(jmethodID id, va_list args) {
    fjni_log_dbg("dummy_void");
}

/* --- Misc --- */

static jboolean hasLowPerformance(jmethodID id, va_list args) {
    return setting_lowPerformance ? JNI_TRUE : JNI_FALSE;
}

static jobject getLanguage(jmethodID id, va_list args) {
    return (jobject)(*(&jni))->NewStringUTF(&jni, "en");
}

static void keepScreenOn(jmethodID id, va_list args) {
    /* no-op on Vita */
}

static jobject getBuildVersion(jmethodID id, va_list args) {
    return (jobject)(*(&jni))->NewStringUTF(&jni, "1.0.0");
}

static jobject getInstallLocation(jmethodID id, va_list args) {
    /* Return as a proper JNI string, since native code calls GetStringUTFChars on it */
    return (jobject)(*(&jni))->NewStringUTF(&jni, DATA_PATH);
}

/* --- FMOD Java compatibility layer --- */

typedef struct {
    int initialized;
    int port;
    int channels;
    int sample_rate;
    int samples_per_buffer;
    int bytes_per_buffer;
    uint8_t *mix_buffer;
} VitaFmodAudioDevice;

static VitaFmodAudioDevice g_fmod_audio_device = { .port = -1 };

static jobject fmod_AudioDevice_ctor(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jobject)&g_fmod_audio_device;
}

static jboolean fmod_checkInit(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    static int logged = 0;
    if (!logged) {
        l_audio("[AUDIO][FMOD] FMOD.checkInit()");
        logged = 1;
    }
    return JNI_TRUE;
}

static jboolean fmod_supportsLowLatency(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    static int logged = 0;
    if (!logged) {
        l_audio("[AUDIO][FMOD] FMOD.supportsLowLatency()");
        logged = 1;
    }
    return JNI_FALSE;
}

static jboolean fmod_supportsAAudio(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    static int logged = 0;
    if (!logged) {
        l_audio("[AUDIO][FMOD] FMOD.supportsAAudio()");
        logged = 1;
    }
    return JNI_FALSE;
}

static jint fmod_getOutputBlockSize(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    static int logged = 0;
    if (!logged) {
        l_audio("[AUDIO][FMOD] FMOD.getOutputBlockSize() -> 1024");
        logged = 1;
    }
    return 1024;
}

static jint fmod_getOutputSampleRate(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    static int logged = 0;
    if (!logged) {
        l_audio("[AUDIO][FMOD] FMOD.getOutputSampleRate() -> 24000");
        logged = 1;
    }
    return 24000;
}

static jint fmod_androidPlaybackStreamType(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    static int logged = 0;
    if (!logged) {
        l_audio("[AUDIO][FMOD] FMOD.androidPlaybackStreamType() -> 3");
        logged = 1;
    }
    return 3; /* Android STREAM_MUSIC */
}

static jint fmod_androidRecordingPreset(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    static int logged = 0;
    if (!logged) {
        l_audio("[AUDIO][FMOD] FMOD.androidRecordingPreset() -> 1");
        logged = 1;
    }
    return 1; /* Android MediaRecorder.AudioSource.DEFAULT */
}

static jboolean fmod_audio_init(jmethodID id, va_list args) {
    (void)id;
    int a0 = va_arg(args, int);
    int a1 = va_arg(args, int);
    int a2 = va_arg(args, int);
    int a3 = va_arg(args, int);

    int channels = 2;
    int sample_rate = 48000;
    int block_size = 1024;
    int num_blocks = 4;

    if ((a0 == 1 || a0 == 2) && a1 >= 8000) {
        channels = a0;
        sample_rate = a1;
        block_size = a2;
        num_blocks = a3;
    } else if ((a1 == 1 || a1 == 2) && a0 >= 8000) {
        channels = a1;
        sample_rate = a0;
        block_size = a2;
        num_blocks = a3;
    } else {
        sample_rate = (a0 >= 8000) ? a0 : sample_rate;
        channels = (a1 == 1 || a1 == 2) ? a1 : channels;
        block_size = (a2 > 0) ? a2 : block_size;
        num_blocks = (a3 > 0) ? a3 : num_blocks;
    }

    if (channels != 1 && channels != 2)
        channels = 2;
    if (sample_rate < 8000 || sample_rate > 48000)
        sample_rate = 48000;
    if (block_size < 64)
        block_size = 1024;
    block_size = (block_size + 63) & ~63;

    if (g_fmod_audio_device.port >= 0) {
        sceAudioOutReleasePort(g_fmod_audio_device.port);
        g_fmod_audio_device.port = -1;
    }
    if (g_fmod_audio_device.mix_buffer) {
        free(g_fmod_audio_device.mix_buffer);
        g_fmod_audio_device.mix_buffer = NULL;
    }

    int mode = (channels == 1) ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, block_size, sample_rate, mode);
    if (port < 0) {
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, block_size, 48000, mode);
        if (port >= 0)
            sample_rate = 48000;
    }
    if (port < 0) {
        l_audio("[AUDIO][FMOD] AudioDevice.init failed openPort raw=(%d,%d,%d,%d) err=0x%08X",
                a0, a1, a2, a3, (unsigned int)port);
        g_fmod_audio_device.initialized = 0;
        return JNI_FALSE;
    }

    int bytes_per_buffer = block_size * channels * (int)sizeof(int16_t);
    uint8_t *mix_buffer = memalign(64, bytes_per_buffer);
    if (!mix_buffer) {
        sceAudioOutReleasePort(port);
        l_audio("[AUDIO][FMOD] AudioDevice.init failed memalign(%d)", bytes_per_buffer);
        g_fmod_audio_device.initialized = 0;
        return JNI_FALSE;
    }
    memset(mix_buffer, 0, bytes_per_buffer);

    g_fmod_audio_device.port = port;
    g_fmod_audio_device.channels = channels;
    g_fmod_audio_device.sample_rate = sample_rate;
    g_fmod_audio_device.samples_per_buffer = block_size;
    g_fmod_audio_device.bytes_per_buffer = bytes_per_buffer;
    g_fmod_audio_device.mix_buffer = mix_buffer;
    g_fmod_audio_device.initialized = 1;

    int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    l_audio("[AUDIO][FMOD] AudioDevice.init raw=(%d,%d,%d,%d) cfg=(sr=%d ch=%d samples=%d buffers=%d port=%d)",
            a0, a1, a2, a3, sample_rate, channels, block_size, num_blocks, port);
    return JNI_TRUE;
}

static void fmod_global_init(jmethodID id, va_list args) {
    (void)id;
    jobject ctx = va_arg(args, jobject);
    l_audio("[AUDIO][FMOD] FMOD.init(ctx=%p)", ctx);
}

static void fmod_audio_close(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    if (g_fmod_audio_device.port >= 0) {
        sceAudioOutReleasePort(g_fmod_audio_device.port);
        g_fmod_audio_device.port = -1;
    }
    if (g_fmod_audio_device.mix_buffer) {
        free(g_fmod_audio_device.mix_buffer);
        g_fmod_audio_device.mix_buffer = NULL;
    }
    g_fmod_audio_device.initialized = 0;
    l_audio("[AUDIO][FMOD] AudioDevice.close()");
}

static int fmod_audio_write_impl(JavaDynArray *data, int length) {
    if (!data || !data->array || length <= 0)
        return 0;
    if (!g_fmod_audio_device.initialized || g_fmod_audio_device.port < 0 ||
        !g_fmod_audio_device.mix_buffer || g_fmod_audio_device.bytes_per_buffer <= 0)
        return 0;

    const uint8_t *src = (const uint8_t *)data->array;

#ifdef ENABLE_AUDIO_LOGS
    int peak = 0;
    int avg = 0;
    int64_t abs_sum = 0;
    int sample_count = length / (int)sizeof(int16_t);
    const int16_t *samples = (const int16_t *)src;
    for (int i = 0; i < sample_count; ++i) {
        int v = samples[i];
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
        abs_sum += v;
    }
    avg = sample_count > 0 ? (int)(abs_sum / sample_count) : 0;
#endif

    int remaining = length;
    int written = 0;
    while (remaining > 0) {
        int copy = remaining;
        if (copy > g_fmod_audio_device.bytes_per_buffer)
            copy = g_fmod_audio_device.bytes_per_buffer;

        memcpy(g_fmod_audio_device.mix_buffer, src, copy);
        if (copy < g_fmod_audio_device.bytes_per_buffer) {
            memset(g_fmod_audio_device.mix_buffer + copy, 0,
                   g_fmod_audio_device.bytes_per_buffer - copy);
        }

        int out = sceAudioOutOutput(g_fmod_audio_device.port, g_fmod_audio_device.mix_buffer);
        if (out < 0) {
            l_audio("[AUDIO][FMOD] AudioDevice.write output err=0x%08X", (unsigned int)out);
            break;
        }

        src += copy;
        remaining -= copy;
        written += copy;
    }

#ifdef ENABLE_AUDIO_LOGS
    static uint32_t write_count = 0;
    write_count++;
    if (write_count <= 12 || (write_count % 180) == 0) {
        l_audio("[AUDIO][FMOD] AudioDevice.write#%u(len=%d peak=%d avg=%d written=%d)",
                write_count, length, peak, avg, written);
    }
#endif

    return written;
}

static void fmod_audio_write(jmethodID id, va_list args) {
    (void)id;
    JavaDynArray *data = va_arg(args, JavaDynArray *);
    int length = va_arg(args, int);
    (void)fmod_audio_write_impl(data, length);
}

static jint fmod_audio_write_int(jmethodID id, va_list args) {
    (void)id;
    JavaDynArray *data = va_arg(args, JavaDynArray *);
    int length = va_arg(args, int);
    return (jint)fmod_audio_write_impl(data, length);
}

/* --- FalsoJNI dispatch tables --- */

MethodsBoolean methodsBoolean[] = {
    { MID_FH_IS_TRIAL,            fh_IsTrial },
    { MID_FH_ACCESS_EVERY_MISSION, fh_AccessEveryMission },
    { MID_HAS_LOW_PERFORMANCE,    hasLowPerformance },
    { MID_FMOD_CHECK_INIT,        fmod_checkInit },
    { MID_FMOD_SUPPORTS_LL,       fmod_supportsLowLatency },
    { MID_FMOD_SUPPORTS_AAUDIO,   fmod_supportsAAudio },
    { MID_FMOD_AUDIO_INIT,        fmod_audio_init },
};

MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};

MethodsLong methodsLong[] = {
    { MID_APK_SEEK_FILE, apk_seekFileAndroid },
};

MethodsShort methodsShort[] = {};

MethodsInt methodsInt[] = {
    { MID_FH_GET_LANGUAGE_ID,  fh_GetLanguageID },
    { MID_FH_GET_DATE_YEAR,    fh_GetDate_Year },
    { MID_FH_GET_DATE_MONTH,   fh_GetDate_Month },
    { MID_FH_GET_DATE_DAY,     fh_GetDate_Day },
    { MID_FMOD_GET_OUT_BLOCK,  fmod_getOutputBlockSize },
    { MID_FMOD_GET_OUT_SR,     fmod_getOutputSampleRate },
    { MID_FMOD_PLAYBACK_STREAM, fmod_androidPlaybackStreamType },
    { MID_FMOD_RECORDING_PRESET, fmod_androidRecordingPreset },
    { MID_FMOD_AUDIO_WRITE,    fmod_audio_write_int },
};

MethodsObject methodsObject[] = {
    { MID_APK_GET_INSTANCE,    apk_getInstance },
    { MID_APK_OPEN_FILE,       apk_openFileAndroid },
    /* getInstance shared: APKFileHelper and FileHelper both return non-null */
    { MID_FH_OPEN_INPUT,       fh_openInputFile },
    { MID_FH_OPEN_OUTPUT,      fh_openOutputFile },
    { MID_GET_INSTALL_LOCATION, getInstallLocation },
    { MID_GET_LANGUAGE,         getLanguage },
    { MID_GET_BUILD_VERSION,    getBuildVersion },
    { MID_FMOD_AUDIO_CTOR,      fmod_AudioDevice_ctor },
};

MethodsVoid methodsVoid[] = {
    { MID_APK_CLOSE_FILE,      apk_closeFileAndroid },
    { MID_APK_READ_FILE,       apk_readFileAndroid },
    { MID_FH_READ_FILE,        fh_readFile },
    { MID_FH_WRITE_FILE,       fh_writeFile },
    { MID_FH_CLOSE_INPUT,      fh_closeInputFile },
    { MID_FH_CLOSE_OUTPUT,     fh_closeOutputFile },
    { MID_FH_WRITE_FILE_SD,    fh_writeFileToSD },
    { MID_RATE_SET_MSG,        rate_SetRateMsg },
    { MID_SET_RATE_APP_TEXTS,  dummy_void },
    { MID_KEEP_SCREEN_ON,      keepScreenOn },
    { MID_FMOD_AUDIO_INIT,     fmod_global_init },
    { MID_FMOD_AUDIO_CLOSE,    fmod_audio_close },
    { MID_FMOD_AUDIO_WRITE,    fmod_audio_write },
};

/*
 * JNI Fields
 *
 * The native code accesses APKFile fields via GetIntField/GetObjectField.
 * FalsoJNI's field system is global (ignores the object parameter), so we
 * use global state updated by the APKFileHelper methods.
 *
 * IMPORTANT: The field values for length/position/data are dynamically
 * updated by our method implementations. We provide getters that read
 * from g_current_apk_file.
 *
 * Since FalsoJNI's field system uses static values, we handle dynamic
 * fields by hooking into the field access path. The initial values here
 * serve as defaults; the actual values are updated by our method impls
 * via the set*FieldValueById functions.
 */

static char WINDOW_SERVICE[] = "window";

NameToFieldID nameToFieldId[] = {
    { FID_LENGTH,        "length",         FIELD_TYPE_INT },
    { FID_POSITION,      "position",       FIELD_TYPE_INT },
    { FID_DATA,          "data",           FIELD_TYPE_OBJECT },
    { FID_BUFFER_SIZE,   "bufferSize",     FIELD_TYPE_INT },
    { FID_BUFFER,        "Buffer",         FIELD_TYPE_OBJECT },
    { FID_SDK_INT,       "SDK_INT",        FIELD_TYPE_INT },
    { FID_WINDOW_SERVICE, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
    { FID_S_LANGUAGE_ID, "s_languageID",   FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
    { FID_LENGTH,       0 },
    { FID_POSITION,     0 },
    { FID_BUFFER_SIZE,  1024 },
    { FID_SDK_INT,      14 },
    { FID_S_LANGUAGE_ID, -1 },
};
FieldsObject fieldsObject[] = {
    { FID_DATA,          NULL },
    { FID_BUFFER,        NULL },
    { FID_WINDOW_SERVICE, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES

/*
 * Hook: called after APKFileHelper methods to sync field values.
 * We override the FalsoJNI field getters by updating the static tables
 * whenever the current APKFile changes.
 *
 * This is called from our method implementations above.
 */
void java_update_apk_fields(void) {
    if (g_current_apk_file) {
        setIntFieldValueById((jfieldID)FID_LENGTH, g_current_apk_file->length);
        setIntFieldValueById((jfieldID)FID_POSITION, g_current_apk_file->position);
        setIntFieldValueById((jfieldID)FID_BUFFER_SIZE, g_current_apk_file->bufferSize);
        setObjectFieldValueById((jfieldID)FID_DATA, (jobject)g_current_apk_file->data);
    }
    if (g_file_helper_buffer) {
        setObjectFieldValueById((jfieldID)FID_BUFFER, (jobject)g_file_helper_buffer);
    }
}
