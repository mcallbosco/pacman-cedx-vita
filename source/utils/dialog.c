/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      fgsfds
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/dialog.h"

#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>

#include <vitaGL.h>

static uint16_t ime_title_utf16[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static uint16_t ime_initial_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH];
static uint16_t ime_input_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint8_t ime_input_text_utf8[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static SceMsgDialogButtonsParam msg_buttons;
static char msg_text[SCE_MSG_DIALOG_USER_MSG_SIZE];
static char msg_btn1[24];
static char msg_btn2[24];
static char msg_btn3[24];

static void copy_msg_text(char *dst, size_t dst_size, const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    sceClibSnprintf(dst, dst_size, "%s", src);
}

void _utf16_to_utf8(const uint16_t *src, uint8_t *dst) {
    for (int i = 0; src[i]; i++) {
        if ((src[i] & 0xFF80) == 0) {
            *(dst++) = src[i] & 0xFF;
        } else if((src[i] & 0xF800) == 0) {
            *(dst++) = ((src[i] >> 6) & 0xFF) | 0xC0;
            *(dst++) = (src[i] & 0x3F) | 0x80;
        } else if((src[i] & 0xFC00) == 0xD800 && (src[i + 1] & 0xFC00) == 0xDC00) {
            *(dst++) = (((src[i] + 64) >> 8) & 0x3) | 0xF0;
            *(dst++) = (((src[i] >> 2) + 16) & 0x3F) | 0x80;
            *(dst++) = ((src[i] >> 4) & 0x30) | 0x80 | ((src[i + 1] << 2) & 0xF);
            *(dst++) = (src[i + 1] & 0x3F) | 0x80;
            i += 1;
        } else {
            *(dst++) = ((src[i] >> 12) & 0xF) | 0xE0;
            *(dst++) = ((src[i] >> 6) & 0x3F) | 0x80;
            *(dst++) = (src[i] & 0x3F) | 0x80;
        }
    }

    *dst = '\0';
}

void _utf8_to_utf16(const uint8_t *src, uint16_t *dst) {
    for (int i = 0; src[i];) {
        if ((src[i] & 0xE0) == 0xE0) {
            *(dst++) = ((src[i]&0x0F) <<12) | ((src[i+1]&0x3F) <<6) | (src[i+2]&0x3F);
            i += 3;
        } else if ((src[i] & 0xC0) == 0xC0) {
            *(dst++) = ((src[i] & 0x1F) << 6) | (src[i + 1] & 0x3F);
            i += 2;
        } else {
            *(dst++) = src[i];
            i += 1;
        }
    }

    *dst = '\0';
}

int init_ime_dialog(const char *title, const char *initial_text) {
    sceClibMemset(ime_title_utf16, 0, sizeof(ime_title_utf16));
    sceClibMemset(ime_initial_text_utf16, 0, sizeof(ime_initial_text_utf16));
    sceClibMemset(ime_input_text_utf16, 0, sizeof(ime_input_text_utf16));
    sceClibMemset(ime_input_text_utf8, 0, sizeof(ime_input_text_utf8));

    _utf8_to_utf16((uint8_t *)title, ime_title_utf16);
    _utf8_to_utf16((uint8_t *)initial_text, ime_initial_text_utf16);

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);

    param.supportedLanguages = 0x0001FFFF;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.title = ime_title_utf16;
    param.maxTextLength = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    param.initialText = ime_initial_text_utf16;
    param.inputTextBuffer = ime_input_text_utf16;

    return sceImeDialogInit(&param);
}

char *get_ime_dialog_result(void) {
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return NULL;

    SceImeDialogResult result;
    sceClibMemset(&result, 0, sizeof(SceImeDialogResult));
    sceImeDialogGetResult(&result);
    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER)
        _utf16_to_utf8(ime_input_text_utf16, ime_input_text_utf8);
    sceImeDialogTerm();
    // For some reason analog stick stops working after ime
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

    return (char *)ime_input_text_utf8;
}

int init_msg_dialog(const char *msg) {
    SceMsgDialogUserMessageParam msg_param;
    sceClibMemset(&msg_param, 0, sizeof(msg_param));
    msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    copy_msg_text(msg_text, sizeof(msg_text), msg);
    msg_param.msg = (SceChar8 *)msg_text;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    _sceCommonDialogSetMagicNumber(&param.commonParam);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg_param;

    return sceMsgDialogInit(&param);
}

int init_msg_dialog_3buttons(const char *msg,
                             const char *button1,
                             const char *button2,
                             const char *button3) {
    sceClibMemset(&msg_buttons, 0, sizeof(msg_buttons));
    copy_msg_text(msg_btn1, sizeof(msg_btn1), button1);
    copy_msg_text(msg_btn2, sizeof(msg_btn2), button2);
    copy_msg_text(msg_btn3, sizeof(msg_btn3), button3);
    msg_buttons.msg1 = msg_btn1;
    msg_buttons.msg2 = msg_btn2;
    msg_buttons.msg3 = msg_btn3;
    msg_buttons.fontSize1 = SCE_MSG_DIALOG_FONT_SIZE_DEFAULT;
    msg_buttons.fontSize2 = SCE_MSG_DIALOG_FONT_SIZE_DEFAULT;
    msg_buttons.fontSize3 = SCE_MSG_DIALOG_FONT_SIZE_DEFAULT;

    SceMsgDialogUserMessageParam msg_param;
    sceClibMemset(&msg_param, 0, sizeof(msg_param));
    msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_3BUTTONS;
    copy_msg_text(msg_text, sizeof(msg_text), msg);
    msg_param.msg = (SceChar8 *)msg_text;
    msg_param.buttonParam = &msg_buttons;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    _sceCommonDialogSetMagicNumber(&param.commonParam);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg_param;

    return sceMsgDialogInit(&param);
}

int get_msg_dialog_result(void) {
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;
    sceMsgDialogTerm();
    return 1;
}

int get_msg_dialog_button_id(void) {
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;

    SceMsgDialogResult result;
    sceClibMemset(&result, 0, sizeof(result));
    sceMsgDialogGetResult(&result);
    sceMsgDialogTerm();
    return result.buttonId;
}

void fatal_error(const char *fmt, ...) {
    va_list list;
    char string[512];

    va_start(list, fmt);
    sceClibVsnprintf(string, sizeof(string), fmt, list);
    va_end(list);

    vglInit(0);

    init_msg_dialog(string);

    while (!get_msg_dialog_result())
        vglSwapBuffers(GL_TRUE);

    sceKernelExitProcess(0);

    sceKernelExitProcess(0);
    while (1);
}
