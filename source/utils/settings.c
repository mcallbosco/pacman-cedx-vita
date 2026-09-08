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
int  setting_frameRate;
int  setting_resolution;
bool setting_nativeUi;
int  setting_buildType;
bool setting_lowPerformance;
bool setting_unlockAllContent;
int setting_motionBlurSamples;
bool setting_reduceGhostTrails;
bool setting_ghostChainTrails;
bool setting_ghostEyeTrails;
bool setting_mazeWobble;
bool setting_dangerZoom;
bool setting_motionBlur;
bool setting_ghostAfterimages;
bool setting_ghostEatOutline;
bool setting_pacmanLight;
bool setting_powerPalette;
bool setting_powerFlash;
bool setting_powerPulse;
bool setting_mazeGlow;
bool setting_impactRipples;
bool setting_ghostEatParticles;
bool setting_ghostLights;
bool setting_pcSpeed;
bool setting_pcRules;
int setting_language;

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
    setting_language = SETTING_LANGUAGE_SYSTEM;
    setting_pcSpeed = true;
    setting_pcRules = true;
    setting_sampleSetting  = 1;
    setting_sampleSetting2 = true;
    setting_msaaMode       = SETTING_MSAA_OFF;
    setting_frameRate      = 60;
    setting_resolution     = SETTING_RESOLUTION_NATIVE;
    setting_nativeUi       = true;
    setting_buildType      = 0;
    setting_lowPerformance = false;
    setting_unlockAllContent = false;
    setting_motionBlurSamples = 4;
    setting_reduceGhostTrails = true;
    setting_ghostChainTrails = false;
    setting_ghostEyeTrails = false;
    setting_mazeWobble = false;
    setting_dangerZoom = false;
    setting_motionBlur = false;
    setting_ghostAfterimages = false;
    setting_ghostEatOutline = true;
    setting_pacmanLight = false;
    setting_powerPalette = false;
    setting_powerFlash = false;
    setting_powerPulse = false;
    setting_mazeGlow = false;
    setting_impactRipples = false;
    setting_ghostEatParticles = false;
    setting_ghostLights = false;
}

void settings_load() {
    settings_reset();

    char buffer[64];
    int value;

    FILE *config = fopen(CONFIG_FILE_PATH, "r");

    if (config) {
        while (fscanf(config, "%63s %d\n", buffer, &value) == 2) {
            if 		(strcmp("setting_sampleSetting", buffer) == 0) 	setting_sampleSetting  = (int)value;
            else if (strcmp("setting_sampleSetting2", buffer) == 0) setting_sampleSetting2 = (bool)value;
            else if (strcmp("setting_msaaMode", buffer) == 0)       setting_msaaMode = settings_sanitize_msaa_mode(value);
            else if (strcmp("setting_frameRate", buffer) == 0)      setting_frameRate = settings_sanitize_frame_rate(value);
            else if (strcmp("setting_resolution", buffer) == 0)     setting_resolution = settings_sanitize_resolution(value);
            else if (strcmp("setting_nativeUi", buffer) == 0)       setting_nativeUi = (bool)value;
            else if (strcmp("setting_buildType", buffer) == 0)      setting_buildType = (value == 0 || value == 1) ? value : 0;
            else if (strcmp("setting_lowPerformance", buffer) == 0) setting_lowPerformance = (bool)value;
            else if (strcmp("setting_unlockAllContent", buffer) == 0 ||
                     strcmp("setting_accessAllMissions", buffer) == 0) setting_unlockAllContent = (bool)value;
            else if (strcmp("setting_motionBlurSamples", buffer) == 0) setting_motionBlurSamples = settings_sanitize_motion_blur_samples(value);
            else if (strcmp("setting_reduceGhostTrails", buffer) == 0) setting_reduceGhostTrails = (bool)value;
            else if (strcmp("setting_ghostChainTrails", buffer) == 0) setting_ghostChainTrails = (bool)value;
            else if (strcmp("setting_ghostEyeTrails", buffer) == 0) setting_ghostEyeTrails = (bool)value;
            else if (strcmp("setting_mazeWobble", buffer) == 0) setting_mazeWobble = (bool)value;
            else if (strcmp("setting_dangerZoom", buffer) == 0) setting_dangerZoom = (bool)value;
            else if (strcmp("setting_motionBlur", buffer) == 0) setting_motionBlur = (bool)value;
            else if (strcmp("setting_ghostAfterimages", buffer) == 0) setting_ghostAfterimages = (bool)value;
            else if (strcmp("setting_ghostEatOutline", buffer) == 0) setting_ghostEatOutline = (bool)value;
            else if (strcmp("setting_pacmanLight", buffer) == 0) setting_pacmanLight = (bool)value;
            else if (strcmp("setting_powerPalette", buffer) == 0) setting_powerPalette = (bool)value;
            else if (strcmp("setting_powerFlash", buffer) == 0) setting_powerFlash = (bool)value;
            else if (strcmp("setting_powerPulse", buffer) == 0) setting_powerPulse = (bool)value;
            else if (strcmp("setting_mazeGlow", buffer) == 0) setting_mazeGlow = (bool)value;
            else if (strcmp("setting_impactRipples", buffer) == 0) setting_impactRipples = (bool)value;
            else if (strcmp("setting_ghostEatParticles", buffer) == 0) setting_ghostEatParticles = (bool)value;
            else if (strcmp("setting_pcRules", buffer) == 0) setting_pcRules = (bool)value;
            else if (strcmp("setting_pcSpeed", buffer) == 0) setting_pcSpeed = (bool)value;
            else if (strcmp("setting_language", buffer) == 0) setting_language = settings_sanitize_language(value);
        }
        fclose(config);
    }

    setting_msaaMode = settings_sanitize_msaa_mode(setting_msaaMode);
}

void settings_apply_runtime_overrides() {
    if (!setting_lowPerformance)
        return;
    /* The configurator retains saved preferences. These values are used only
     * by this game launch, including settings from older configuration files. */
    setting_msaaMode = SETTING_MSAA_OFF;
    setting_nativeUi = false;
    setting_frameRate = 60;
    setting_motionBlur = false;
    setting_motionBlurSamples = 2;
    setting_reduceGhostTrails = true;
    setting_ghostAfterimages = false;
    setting_ghostChainTrails = false;
    setting_ghostEyeTrails = false;
    setting_mazeWobble = false;
    setting_dangerZoom = false;
    setting_ghostEatOutline = false;
    setting_pacmanLight = false;
    setting_powerPalette = false;
    setting_powerFlash = false;
    setting_powerPulse = false;
    setting_mazeGlow = false;
    setting_impactRipples = false;
    setting_ghostEatParticles = false;
    setting_ghostLights = false;
}

void settings_save() {
    FILE *config = fopen(CONFIG_FILE_PATH, "w+");

    if (config) {
        fprintf(config, "setting_language %d\n", settings_sanitize_language(setting_language));
        fprintf(config, "setting_pcRules %d\n", (int)setting_pcRules);
        fprintf(config, "setting_pcSpeed %d\n", (int)setting_pcSpeed);
        fprintf(config, "%s %d\n", "setting_sampleSetting", (int)(setting_sampleSetting));
        fprintf(config, "%s %d\n", "setting_sampleSetting2", (int)(setting_sampleSetting2));
        fprintf(config, "%s %d\n", "setting_msaaMode", settings_sanitize_msaa_mode(setting_msaaMode));
        fprintf(config, "setting_frameRate %d\n", settings_sanitize_frame_rate(setting_frameRate));
        fprintf(config, "setting_resolution %d\n", settings_sanitize_resolution(setting_resolution));
        fprintf(config, "setting_nativeUi %d\n", (int)setting_nativeUi);
        fprintf(config, "%s %d\n", "setting_buildType", (setting_buildType == 0 || setting_buildType == 1) ? setting_buildType : 0);
        fprintf(config, "%s %d\n", "setting_lowPerformance", (int)(setting_lowPerformance));
        fprintf(config, "%s %d\n", "setting_unlockAllContent", (int)(setting_unlockAllContent));
        fprintf(config, "setting_motionBlurSamples %d\n", settings_sanitize_motion_blur_samples(setting_motionBlurSamples));
        fprintf(config, "setting_reduceGhostTrails %d\n", (int)setting_reduceGhostTrails);
        fprintf(config, "setting_ghostChainTrails %d\n", (int)setting_ghostChainTrails);
        fprintf(config, "setting_ghostEyeTrails %d\n", (int)setting_ghostEyeTrails);
        fprintf(config, "setting_mazeWobble %d\n", (int)setting_mazeWobble);
        fprintf(config, "setting_dangerZoom %d\n", (int)setting_dangerZoom);
        fprintf(config, "setting_motionBlur %d\n", (int)setting_motionBlur);
        fprintf(config, "setting_ghostAfterimages %d\n", (int)setting_ghostAfterimages);
        fprintf(config, "setting_ghostEatOutline %d\n", (int)setting_ghostEatOutline);
        fprintf(config, "setting_pacmanLight %d\n", (int)setting_pacmanLight);
        fprintf(config, "setting_powerPalette %d\n", (int)setting_powerPalette);
        fprintf(config, "setting_powerFlash %d\n", (int)setting_powerFlash);
        fprintf(config, "setting_powerPulse %d\n", (int)setting_powerPulse);
        fprintf(config, "setting_mazeGlow %d\n", (int)setting_mazeGlow);
        fprintf(config, "setting_impactRipples %d\n", (int)setting_impactRipples);
        fprintf(config, "setting_ghostEatParticles %d\n", (int)setting_ghostEatParticles);
        fclose(config);
    }
}
