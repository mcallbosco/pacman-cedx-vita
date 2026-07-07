/*
 * Copyright (C) 2025 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/controls.h"
#include "utils/logger.h"

#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <psp2/kernel/clib.h>

#define LEFT_ANALOG_DEADZONE  0.16f
#define RIGHT_ANALOG_DEADZONE 0.16f

#define TOUCH_SCREEN_WIDTH  960.0f
#define TOUCH_SCREEN_HEIGHT 544.0f

static SceTouchPanelInfo front_touch_info;
static int front_touch_info_valid = 0;

void coord_normalize(float * x, float * y, float deadzone) {
    float magnitude = sqrtf((*x * *x) + (*y * *y));
    if (magnitude < deadzone) {
        *x = 0;
        *y = 0;
        return;
    }

    // normalize
    *x = *x / magnitude;
    *y = *y / magnitude;

    float multiplier = ((magnitude - deadzone) / (1 - deadzone));
    *x = *x * multiplier;
    *y = *y * multiplier;
}

void controls_init() {
    // Enable analog sticks and touchscreen
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    int ret = sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    l_info("sceTouchSetSamplingState(front): %d", ret);

    ret = sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &front_touch_info);
    if (ret >= 0) {
        front_touch_info_valid = 1;
        l_info("front touch panel: active=(%d,%d)-(%d,%d) display=(%d,%d)-(%d,%d)",
               front_touch_info.minAaX, front_touch_info.minAaY,
               front_touch_info.maxAaX, front_touch_info.maxAaY,
               front_touch_info.minDispX, front_touch_info.minDispY,
               front_touch_info.maxDispX, front_touch_info.maxDispY);
    } else {
        l_warn("sceTouchGetPanelInfo(front) failed: %d; using 1920x1088 fallback", ret);
    }

    // Enable accelerometer
    sceMotionStartSampling();
}

void poll_touch();
void poll_pad();
void poll_accel();

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone);

void controls_poll() {
    controls_poll_touch();
    poll_pad();
    //poll_accel();
}

SceTouchData touch;
SceTouchData touch_old;

static float touch_scale_x(int raw_x) {
    if (front_touch_info_valid && front_touch_info.maxDispX > front_touch_info.minDispX) {
        return ((float)(raw_x - front_touch_info.minDispX) * TOUCH_SCREEN_WIDTH) /
               (float)(front_touch_info.maxDispX - front_touch_info.minDispX);
    }

    return (float)raw_x * TOUCH_SCREEN_WIDTH / 1920.0f;
}

static float touch_scale_y(int raw_y) {
    if (front_touch_info_valid && front_touch_info.maxDispY > front_touch_info.minDispY) {
        return ((float)(raw_y - front_touch_info.minDispY) * TOUCH_SCREEN_HEIGHT) /
               (float)(front_touch_info.maxDispY - front_touch_info.minDispY);
    }

    return (float)raw_y * TOUCH_SCREEN_HEIGHT / 1088.0f;
}

void controls_poll_touch() {
    poll_touch();
}

void poll_touch() {
    int ret = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    if (ret < 0) {
        l_warn("sceTouchPeek(front) failed: %d", ret);
        return;
    }

    for (int i = 0; i < touch.reportNum; i++) {
        float x = touch_scale_x(touch.report[i].x);
        float y = touch_scale_y(touch.report[i].y);

        // Check if the finger was down before to distinguish between the Move and Down events
        int finger_down = 0;

        if (touch_old.reportNum > 0) {
            for (int j = 0; j < touch_old.reportNum; j++) {
                if (touch.report[i].id == touch_old.report[j].id) {
                    finger_down = 1;
                    break;
                }
            }
        }

        if (!finger_down) {
            controls_handler_touch(touch.report[i].id, x, y, CONTROLS_ACTION_DOWN);
        } else {
            controls_handler_touch(touch.report[i].id, x, y, CONTROLS_ACTION_MOVE);
        }
    }

    for (int i = 0; i < touch_old.reportNum; i++) {
        int finger_up = 1;

        for (int j = 0; j < touch.reportNum; j++) {
            if (touch.report[j].id == touch_old.report[i].id ) {
                finger_up = 0;
                break;
            }
        }

        if (finger_up == 1) {
            float x = touch_scale_x(touch_old.report[i].x);
            float y = touch_scale_y(touch_old.report[i].y);

            controls_handler_touch(touch_old.report[i].id, x, y, CONTROLS_ACTION_UP);
        }
    }

    sceClibMemcpy(&touch_old, &touch, sizeof(touch));
}

static ButtonMapping mapping[] = {
        { SCE_CTRL_UP,        AKEYCODE_DPAD_UP },
        { SCE_CTRL_DOWN,      AKEYCODE_DPAD_DOWN },
        { SCE_CTRL_LEFT,      AKEYCODE_DPAD_LEFT },
        { SCE_CTRL_RIGHT,     AKEYCODE_DPAD_RIGHT },
        { SCE_CTRL_CROSS,     AKEYCODE_BUTTON_A },
        { SCE_CTRL_SQUARE,    AKEYCODE_BUTTON_X },
        { SCE_CTRL_TRIANGLE,  AKEYCODE_BUTTON_Y },
        { SCE_CTRL_L1,        AKEYCODE_BUTTON_L1 },
        { SCE_CTRL_R1,        AKEYCODE_BUTTON_R1 },
        { SCE_CTRL_START,     AKEYCODE_BUTTON_START },
        { SCE_CTRL_SELECT,    AKEYCODE_BUTTON_SELECT },
};

uint32_t old_buttons = 0, current_buttons = 0, pressed_buttons = 0, released_buttons = 0;

float analog_lx[3] = { 0 };
float analog_ly[3] = { 0 };
float analog_rx[3] = { 0 };
float analog_ry[3] = { 0 };

void poll_pad() {
    SceCtrlData pad;
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

    // Gamepad buttons
    old_buttons = current_buttons;
    current_buttons = pad.buttons;
    pressed_buttons = current_buttons & ~old_buttons;
    released_buttons = ~current_buttons & old_buttons;

    for (int i = 0; i < sizeof(mapping) / sizeof(ButtonMapping); i++) {
        if (pressed_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_DOWN);
        }
        if (released_buttons & mapping[i].sce_button) {
            controls_handler_key(mapping[i].android_button, CONTROLS_ACTION_UP);
        }
    }

    // Analog sticks
    poll_stick(CONTROLS_STICK_LEFT, (float)pad.lx, (float)pad.ly, analog_lx, analog_ly, LEFT_ANALOG_DEADZONE);
    poll_stick(CONTROLS_STICK_RIGHT, (float)pad.rx, (float)pad.ry, analog_rx, analog_ry, RIGHT_ANALOG_DEADZONE);
}

void poll_stick(ControlsStickId which, float raw_x, float raw_y, float * readings_x, float * readings_y, float deadzone) {
    readings_x[0] = (raw_x - 128.0f) / 128.0f;
    readings_y[0] = (raw_y - 128.0f) / 128.0f;

    coord_normalize(&readings_x[0], &readings_y[0], deadzone);

    // Last two readings are 0, the one before that isn't ==> MOTION_ACTION_UP
    if (
        (readings_x[0] == 0.f && readings_y[0] == 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] != 0.f || readings_y[2] != 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_UP);
    }
    // Current reading isn't 0, the two before are ==> MOTION_ACTION_DOWN
    else if (
        (readings_x[0] != 0.f || readings_y[0] != 0.f) &&
        (readings_x[1] == 0.f && readings_y[1] == 0.f) &&
        (readings_x[2] == 0.f && readings_y[2] == 0.f)
    ) {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_DOWN);
    }
    // Other cases ==> MOTION_ACTION_MOVE
    else {
        controls_handler_analog(which, readings_x[0], readings_y[0], CONTROLS_ACTION_MOVE);
    }

    readings_x[2] = readings_x[1];
    readings_y[2] = readings_y[1];
    readings_x[1] = readings_x[0];
    readings_y[1] = readings_y[0];
}
