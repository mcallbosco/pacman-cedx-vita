/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include "settings.h"

#define CONFIG_FILE_PATH WRITABLE_PATH "config.txt"

int  setting_sampleSetting;
bool setting_sampleSetting2;
int  setting_msaaMode;
int  setting_buildType;
bool setting_lowPerformance;
bool setting_accessAllMissions;

const char *settings_msaa_to_string(int mode) {
    switch (mode) {
        case SETTING_MSAA_OFF: return "OFF";
        case SETTING_MSAA_2X:  return "2X";
        case SETTING_MSAA_4X:  return "4X";
        default: return "2X";
    }
}

int settings_sanitize_msaa_mode(int mode) {
    if (mode != SETTING_MSAA_OFF &&
        mode != SETTING_MSAA_2X &&
        mode != SETTING_MSAA_4X)
        return SETTING_MSAA_OFF;
    return mode;
}

void settings_reset() {
    setting_sampleSetting  = 1;
    setting_sampleSetting2 = true;
    setting_msaaMode       = SETTING_MSAA_OFF;
    setting_buildType      = 0;
    setting_lowPerformance = false;
    setting_accessAllMissions = true;
}

void settings_load() {
    settings_reset();

    char buffer[30];
    int value;

    FILE *config = fopen(CONFIG_FILE_PATH, "r");

    if (config) {
        while (EOF != fscanf(config, "%[^ ] %d\n", buffer, &value)) {
            if 		(strcmp("setting_sampleSetting", buffer) == 0) 	setting_sampleSetting  = (int)value;
            else if (strcmp("setting_sampleSetting2", buffer) == 0) setting_sampleSetting2 = (bool)value;
            else if (strcmp("setting_msaaMode", buffer) == 0)       setting_msaaMode = settings_sanitize_msaa_mode(value);
            else if (strcmp("setting_buildType", buffer) == 0)      setting_buildType = (value == 0 || value == 1) ? value : 0;
            else if (strcmp("setting_lowPerformance", buffer) == 0) setting_lowPerformance = (bool)value;
            else if (strcmp("setting_accessAllMissions", buffer) == 0) setting_accessAllMissions = (bool)value;
        }
        fclose(config);
    }

    setting_msaaMode = settings_sanitize_msaa_mode(setting_msaaMode);
}

void settings_save() {
    FILE *config = fopen(CONFIG_FILE_PATH, "w+");

    if (config) {
        fprintf(config, "%s %d\n", "setting_sampleSetting", (int)(setting_sampleSetting));
        fprintf(config, "%s %d\n", "setting_sampleSetting2", (int)(setting_sampleSetting2));
        fprintf(config, "%s %d\n", "setting_msaaMode", settings_sanitize_msaa_mode(setting_msaaMode));
        fprintf(config, "%s %d\n", "setting_buildType", (setting_buildType == 0 || setting_buildType == 1) ? setting_buildType : 0);
        fprintf(config, "%s %d\n", "setting_lowPerformance", (int)(setting_lowPerformance));
        fprintf(config, "%s %d\n", "setting_accessAllMissions", (int)(setting_accessAllMissions));
        fclose(config);
    }
}
