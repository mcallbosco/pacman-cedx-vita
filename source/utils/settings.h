/*
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  settings.h
 * @brief Loader settings that can be set via a configurator app.
 */

#ifndef SOLOADER_SETTINGS_H
#define SOLOADER_SETTINGS_H

#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SETTING_MSAA_OFF = 0,
    SETTING_MSAA_2X = 1,
    SETTING_MSAA_4X = 2
};

enum {
    SETTING_RESOLUTION_NATIVE = 0,
    SETTING_RESOLUTION_720X408,
    SETTING_RESOLUTION_PSP,
    SETTING_RESOLUTION_COUNT
};

enum {
    SETTING_LANGUAGE_SYSTEM = -1,
    SETTING_LANGUAGE_JAPANESE = 0,
    SETTING_LANGUAGE_ENGLISH,
    SETTING_LANGUAGE_FRENCH,
    SETTING_LANGUAGE_ITALIAN,
    SETTING_LANGUAGE_GERMAN,
    SETTING_LANGUAGE_SPANISH,
    SETTING_LANGUAGE_RUSSIAN,
    SETTING_LANGUAGE_CHINESE,
    SETTING_LANGUAGE_KOREAN,
    SETTING_LANGUAGE_PORTUGUESE,
    SETTING_LANGUAGE_COUNT
};

extern int  setting_sampleSetting;
extern bool setting_sampleSetting2;
extern int  setting_msaaMode;
extern int  setting_frameRate;       /* Render at 30 or 60 FPS. */
extern int  setting_resolution;      /* Display preset, applied at game launch. */
extern int  setting_buildType;       /* 0=release, 1=debug/dev */
extern bool setting_lowPerformance;  /* Native low-performance mode and graphics override. */
extern bool setting_unlockAllContent; /* true = bypass content access checks */
extern int setting_motionBlurSamples; /* 2/4 = reduced, 8 = original */
extern bool setting_reduceGhostTrails;
extern bool setting_ghostChainTrails;
extern bool setting_ghostEyeTrails;
extern bool setting_mazeWobble;
extern bool setting_dangerZoom;
extern bool setting_motionBlur;
extern bool setting_ghostAfterimages;
extern bool setting_ghostEatOutline;
extern bool setting_pacmanLight;
extern bool setting_powerPalette;
extern bool setting_powerFlash;
extern bool setting_powerPulse;
extern bool setting_mazeGlow;
extern bool setting_impactRipples;
extern bool setting_ghostEatParticles;
extern bool setting_ghostLights;      /* Parked effect; not loaded from configuration. */
extern bool setting_pcSpeed;
extern bool setting_pcRules;
extern int setting_language;

static inline int settings_sanitize_language(int language) {
    return language >= SETTING_LANGUAGE_SYSTEM && language < SETTING_LANGUAGE_COUNT
        ? language : SETTING_LANGUAGE_SYSTEM;
}

static inline int settings_sanitize_motion_blur_samples(int samples) {
    return samples == 2 || samples == 8 ? samples : 4;
}

static inline int settings_sanitize_frame_rate(int rate) {
    return rate == 30 ? 30 : 60;
}

static inline int settings_sanitize_resolution(int resolution) {
    return resolution >= SETTING_RESOLUTION_NATIVE && resolution < SETTING_RESOLUTION_COUNT
        ? resolution : SETTING_RESOLUTION_NATIVE;
}

static inline int settings_resolution_width(int resolution) {
    switch (settings_sanitize_resolution(resolution)) {
        case SETTING_RESOLUTION_720X408: return 720;
        case SETTING_RESOLUTION_PSP: return 480;
        default: return 960;
    }
}

static inline int settings_resolution_height(int resolution) {
    switch (settings_sanitize_resolution(resolution)) {
        case SETTING_RESOLUTION_720X408: return 408;
        case SETTING_RESOLUTION_PSP: return 272;
        default: return 544;
    }
}

static inline const char *settings_resolution_to_string(int resolution) {
    switch (settings_sanitize_resolution(resolution)) {
        case SETTING_RESOLUTION_720X408: return "720x408";
        case SETTING_RESOLUTION_PSP: return "PSP (480x272)";
        default: return "960x544 (NATIVE)";
    }
}

const char *settings_msaa_to_string(int mode);
int settings_sanitize_msaa_mode(int mode);

void settings_load();
void settings_save();
void settings_reset();
/* Apply after loading preferences, before installing hooks or initializing GL. */
void settings_apply_runtime_overrides();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SETTINGS_H
