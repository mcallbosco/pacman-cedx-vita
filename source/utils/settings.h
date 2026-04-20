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

extern int  setting_sampleSetting;
extern bool setting_sampleSetting2;
extern int  setting_msaaMode;
extern int  setting_buildType;       /* 0=release, 1=debug/dev */
extern bool setting_lowPerformance;  /* true = tell game this is weak HW */
extern bool setting_accessAllMissions; /* true = unlock all missions */

const char *settings_msaa_to_string(int mode);
int settings_sanitize_msaa_mode(int mode);

void settings_load();
void settings_save();
void settings_reset();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SETTINGS_H
