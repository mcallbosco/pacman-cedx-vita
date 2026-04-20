/*
 * patch.c
 *
 * Runtime patches for PAC-MAN Championship Edition DX's native library.
 *
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <vitasdk.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern so_module so_mod;
extern so_module fmod_mod;
#ifdef __cplusplus
};
#endif

#include "utils/logger.h"
#include "utils/pgxt.h"
#include <stdbool.h>

static int ret0(void) { return 0; }
static int ret1(void) { return 1; }

/* Replacement for pmcedx::GameScreen_Premium::Init() (empty stub in the
 * original binary). The in-game pause icon is a GUIRunnyButtonStateful2
 * stored at [this+0x140] by the GameScreen_Premium constructor and then
 * registered with the MenuScreen container, which renders it each frame.
 * ui::GUIComponent::IsRenderHide() reads byte [this+0x71]; setting that
 * byte to 1 on the pause button makes every subsequent Render skip it. */
static void hide_pause_button_init(void *self) {
    if (!self) return;
    void *button = *(void **)((char *)self + 0x140);
    if (button) {
        *((uint8_t *)button + 0x71) = 1;
    }
}

typedef int FMOD_RESULT;

#define FMOD_ERR_FILE_NOTFOUND 18
#define FMOD_MODE_LOOP_NORMAL 0x00000002u
#define FMOD_MODE_CREATESTREAM 0x00000080u

static so_hook g_hook_fmod_sys_create_sound_cpp;
#ifdef ENABLE_AUDIO_LOGS
static so_hook g_hook_fmod_sys_init;
static so_hook g_hook_fmod_sys_set_output;
static so_hook g_hook_fmod_sys_set_dsp_buffer;
static so_hook g_hook_fmod_sys_play_sound_cpp;
static so_hook g_hook_fmod_sound_release_cpp;
static so_hook g_hook_fmod_cc_set_mode_cpp;
static so_hook g_hook_fmod_cc_set_paused_cpp;
static so_hook g_hook_fmod_cc_set_volume_cpp;
#endif

#ifdef ENABLE_AUDIO_LOGS
static uint32_t g_fmod_create_sound_retries = 0;
static uint32_t g_fmod_create_sound_calls = 0;
static uint32_t g_fmod_sys_init_calls = 0;
static uint32_t g_fmod_sys_set_output_calls = 0;
static uint32_t g_fmod_sys_set_dsp_calls = 0;
static uint32_t g_fmod_play_sound_calls = 0;
static uint32_t g_fmod_sound_release_calls = 0;
static uint32_t g_fmod_cc_set_mode_calls = 0;
static uint32_t g_fmod_cc_set_paused_calls = 0;
static uint32_t g_fmod_cc_set_volume_calls = 0;
#endif

static int copy_probable_path(const void *ptr, char *dst, size_t dst_size) {
    if (!ptr || !dst || dst_size < 2)
        return 0;

    uintptr_t addr = (uintptr_t)ptr;
    if (addr < 0x1000)
        return 0;

    const unsigned char *src = (const unsigned char *)ptr;
    for (size_t i = 0; i + 1 < dst_size; ++i) {
        unsigned char c = src[i];
        if (c == '\0') {
            dst[i] = '\0';
            return 1;
        }
        if (c < 0x20 || c > 0x7E)
            return 0;
        dst[i] = (char)c;
    }

    dst[dst_size - 1] = '\0';
    return 1;
}

/* Swap .mp3 extension to .ogg in-place if present.
 * The game's sound.xml references .mp3 but disk files are .ogg.
 * Fixing this avoids costly failed fopen() retries on Vita I/O. */
static void swap_mp3_to_ogg(char *path, size_t path_size) {
    size_t len = strlen(path);
    if (len >= 4 && path_size > len &&
        path[len - 4] == '.' &&
        (path[len - 3] == 'm' || path[len - 3] == 'M') &&
        (path[len - 2] == 'p' || path[len - 2] == 'P') &&
        path[len - 1] == '3') {
        path[len - 3] = 'o';
        path[len - 2] = 'g';
        path[len - 1] = 'g';
    }
}

static int remap_fmod_asset_path(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size < 2)
        return 0;

    if (strncmp(in, "file:///android_asset/", 22) == 0 ||
        strncmp(in, "file://android_asset/", 21) == 0) {
        const char *suffix = in + ((in[7] == '/') ? 22 : 21);
        if (strncmp(suffix, "assets/", 7) == 0) {
            snprintf(out, out_size, "%s%s", DATA_PATH, suffix);
        } else {
            snprintf(out, out_size, "%sassets/%s", DATA_PATH, suffix);
        }
        swap_mp3_to_ogg(out, out_size);
        return 1;
    }

    size_t data_path_len = strlen(DATA_PATH);
    size_t data_noslash_len = strlen(DATA_PATH_NOSLASH);
    if (strncmp(in, DATA_PATH_NOSLASH, data_noslash_len) == 0) {
        const char *suffix = NULL;
        if (strncmp(in, DATA_PATH, data_path_len) == 0) {
            suffix = in + data_path_len;
        } else if (in[data_noslash_len] == '/') {
            suffix = in + data_noslash_len + 1;
        }

        if (suffix &&
            strncmp(suffix, "assets/", 7) != 0 &&
            strncmp(suffix, "files/", 6) != 0 &&
            strncmp(suffix, "lib/", 4) != 0) {
            snprintf(out, out_size, DATA_PATH "assets/%s", suffix);
            swap_mp3_to_ogg(out, out_size);
            return 1;
        }
        return 0;
    }

    if (strncmp(in, "/assets/", 8) == 0) {
        snprintf(out, out_size, "%sassets/%s", DATA_PATH, in + 8);
        swap_mp3_to_ogg(out, out_size);
        return 1;
    }
    if (strncmp(in, "assets/", 7) == 0) {
        snprintf(out, out_size, "%sassets/%s", DATA_PATH, in + 7);
        swap_mp3_to_ogg(out, out_size);
        return 1;
    }
    if (strncmp(in, "/sound/", 7) == 0) {
        snprintf(out, out_size, "%sassets/sound/%s", DATA_PATH, in + 7);
        swap_mp3_to_ogg(out, out_size);
        return 1;
    }
    if (strncmp(in, "sound/", 6) == 0) {
        snprintf(out, out_size, "%sassets/%s", DATA_PATH, in);
        swap_mp3_to_ogg(out, out_size);
        return 1;
    }
    if (strncmp(in, "/data/", 6) == 0) {
        snprintf(out, out_size, "%sassets/data/%s", DATA_PATH, in + 6);
        swap_mp3_to_ogg(out, out_size);
        return 1;
    }
    if (strncmp(in, "data/", 5) == 0) {
        snprintf(out, out_size, "%sassets/data/%s", DATA_PATH, in + 5);
        swap_mp3_to_ogg(out, out_size);
        return 1;
    }
    if (in[0] == '/') {
        snprintf(out, out_size, "%sassets/%s", DATA_PATH, in + 1);
        swap_mp3_to_ogg(out, out_size);
        return 1;
    }

    return 0;
}

static int apply_bgm5_alias(const char *in, char *out, size_t out_size) {
    static const char *needle = "bgm5_ost_pac_man_ce_";
    const char *hit = strstr(in, needle);
    if (!hit)
        return 0;

    snprintf(out, out_size, "%s", in);
    char *dst_hit = strstr(out, needle);
    if (!dst_hit)
        return 0;

    const char *suffix = dst_hit + strlen(needle);
    snprintf(dst_hit, (size_t)(out + out_size - dst_hit), "bgm5_ost-pac-man-ce_%s", suffix);
    return 1;
}

static FMOD_RESULT retry_create_sound_with_path_fixups(void *system,
                                                       const void *name_or_data,
                                                       uint32_t mode,
                                                       void *exinfo,
                                                       void **sound,
                                                       FMOD_RESULT initial_ret) {
    if (initial_ret != FMOD_ERR_FILE_NOTFOUND || !name_or_data || exinfo != NULL)
        return initial_ret;

    char original_path[512];
    if (!copy_probable_path(name_or_data, original_path, sizeof(original_path)))
        return initial_ret;

    FMOD_RESULT ret = initial_ret;
    char remapped[512];
    char aliased[512];

    if (remap_fmod_asset_path(original_path, remapped, sizeof(remapped))) {
        ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sys_create_sound_cpp,
                          system, remapped, mode, exinfo, sound);
#ifdef ENABLE_AUDIO_LOGS
        g_fmod_create_sound_retries++;
        l_audio("[AUDIO][FMODAPI] createSound retry#%u remap(%s -> %s) ret=%d sound=%p",
                g_fmod_create_sound_retries, original_path, remapped, ret,
                sound ? *sound : NULL);
#endif
        if (ret == 0)
            return ret;

        if (apply_bgm5_alias(remapped, aliased, sizeof(aliased)) &&
            strcmp(aliased, remapped) != 0) {
            ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sys_create_sound_cpp,
                              system, aliased, mode, exinfo, sound);
#ifdef ENABLE_AUDIO_LOGS
            g_fmod_create_sound_retries++;
            l_audio("[AUDIO][FMODAPI] createSound retry#%u alias(%s -> %s) ret=%d sound=%p",
                    g_fmod_create_sound_retries, remapped, aliased, ret,
                    sound ? *sound : NULL);
#endif
            if (ret == 0)
                return ret;
        }
    }

    if (apply_bgm5_alias(original_path, aliased, sizeof(aliased))) {
        ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sys_create_sound_cpp,
                          system, aliased, mode, exinfo, sound);
#ifdef ENABLE_AUDIO_LOGS
        g_fmod_create_sound_retries++;
        l_audio("[AUDIO][FMODAPI] createSound retry#%u alias(%s -> %s) ret=%d sound=%p",
                g_fmod_create_sound_retries, original_path, aliased, ret,
                sound ? *sound : NULL);
#endif
        if (ret == 0)
            return ret;
    }

    return ret;
}

#ifdef ENABLE_AUDIO_LOGS
static FMOD_RESULT fmod_system_init_cpp_hook(void *system, int max_channels, uint32_t flags, void *extra_driver_data) {
    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sys_init,
                                  system, max_channels, flags, extra_driver_data);
    g_fmod_sys_init_calls++;
    l_audio("[AUDIO][FMODAPI] System::init#%u this=%p maxch=%d flags=0x%08X extra=%p ret=%d",
            g_fmod_sys_init_calls, system, max_channels, flags, extra_driver_data, ret);
    return ret;
}

static FMOD_RESULT fmod_system_set_output_cpp_hook(void *system, int output_type) {
    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sys_set_output, system, output_type);
    g_fmod_sys_set_output_calls++;
    l_audio("[AUDIO][FMODAPI] System::setOutput#%u this=%p output=%d ret=%d",
            g_fmod_sys_set_output_calls, system, output_type, ret);
    return ret;
}

static FMOD_RESULT fmod_system_set_dsp_buffer_cpp_hook(void *system, uint32_t buffer_len, int num_buffers) {
    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sys_set_dsp_buffer, system, buffer_len, num_buffers);
    g_fmod_sys_set_dsp_calls++;
    l_audio("[AUDIO][FMODAPI] System::setDSPBufferSize#%u this=%p len=%u num=%d ret=%d",
            g_fmod_sys_set_dsp_calls, system, buffer_len, num_buffers, ret);
    return ret;
}
#endif

/* Check if a path refers to a BGM (music) file — these are large and
 * should be streamed from disk rather than fully loaded into RAM. */
static int is_bgm_path(const void *name_or_data) {
    char buf[512];
    if (!copy_probable_path(name_or_data, buf, sizeof(buf)))
        return 0;
    /* BGM filenames always contain "bgm" (e.g. bgm1_rainbow_5min.ogg) */
    return strstr(buf, "bgm") != NULL;
}

static FMOD_RESULT fmod_system_create_sound_cpp_hook(void *system,
                                                     const void *name_or_data,
                                                     uint32_t mode,
                                                     void *exinfo,
                                                     void **sound) {
    /* For BGM tracks, force FMOD to stream from disk instead of loading
     * the entire file into memory.  This skips the expensive full-file
     * read at init and only reads headers, deferring I/O to playback. */
    uint32_t effective_mode = mode;
    if (is_bgm_path(name_or_data))
        effective_mode |= FMOD_MODE_CREATESTREAM;

    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sys_create_sound_cpp,
                                  system, name_or_data, effective_mode, exinfo, sound);
    if (ret == FMOD_ERR_FILE_NOTFOUND && mode == FMOD_MODE_LOOP_NORMAL && exinfo == NULL) {
        ret = retry_create_sound_with_path_fixups(system, name_or_data, effective_mode, exinfo, sound, ret);
    }
#ifdef ENABLE_AUDIO_LOGS
    g_fmod_create_sound_calls++;
    if (g_fmod_create_sound_calls <= 240 || ret != 0) {
        char path_preview[160];
        const char *path = copy_probable_path(name_or_data, path_preview, sizeof(path_preview))
                               ? path_preview
                               : "<non-path>";
        l_audio("[AUDIO][FMODAPI] System::createSound#%u this=%p data=%p mode=0x%08X exinfo=%p ret=%d sound=%p",
                g_fmod_create_sound_calls, system, name_or_data, mode, exinfo, ret,
                sound ? *sound : NULL);
        if (ret == FMOD_ERR_FILE_NOTFOUND || mode == FMOD_MODE_LOOP_NORMAL) {
            l_audio("[AUDIO][FMODAPI] System::createSound#%u path=%s", g_fmod_create_sound_calls, path);
        }
    }
#endif
    return ret;
}

#ifdef ENABLE_AUDIO_LOGS
static FMOD_RESULT fmod_system_play_sound_cpp_hook(void *system,
                                                   void *sound,
                                                   void *channel_group,
                                                   int paused,
                                                   void **channel) {
    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sys_play_sound_cpp,
                                  system, sound, channel_group, paused, channel);
    g_fmod_play_sound_calls++;
    if (g_fmod_play_sound_calls <= 240 || ret != 0) {
        l_audio("[AUDIO][FMODAPI] System::playSound#%u this=%p sound=%p group=%p paused=%d ret=%d channel=%p",
                g_fmod_play_sound_calls, system, sound, channel_group, paused, ret,
                channel ? *channel : NULL);
    }
    return ret;
}

static FMOD_RESULT fmod_sound_release_cpp_hook(void *sound) {
    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_sound_release_cpp, sound);
    g_fmod_sound_release_calls++;
    if (g_fmod_sound_release_calls <= 240 || ret != 0) {
        l_audio("[AUDIO][FMODAPI] Sound::release#%u sound=%p ret=%d",
                g_fmod_sound_release_calls, sound, ret);
    }
    return ret;
}

static FMOD_RESULT fmod_channel_set_mode_cpp_hook(void *channel_control, uint32_t mode) {
    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_cc_set_mode_cpp, channel_control, mode);
    g_fmod_cc_set_mode_calls++;
    if (g_fmod_cc_set_mode_calls <= 320 || ret != 0) {
        l_audio("[AUDIO][FMODAPI] ChannelControl::setMode#%u cc=%p mode=0x%08X ret=%d",
                g_fmod_cc_set_mode_calls, channel_control, mode, ret);
    }
    return ret;
}

static FMOD_RESULT fmod_channel_set_paused_cpp_hook(void *channel_control, int paused) {
    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_cc_set_paused_cpp, channel_control, paused);
    g_fmod_cc_set_paused_calls++;
    if (g_fmod_cc_set_paused_calls <= 320 || ret != 0) {
        l_audio("[AUDIO][FMODAPI] ChannelControl::setPaused#%u cc=%p paused=%d ret=%d",
                g_fmod_cc_set_paused_calls, channel_control, paused, ret);
    }
    return ret;
}

static FMOD_RESULT fmod_channel_set_volume_cpp_hook(void *channel_control, uint32_t volume_raw) {
    FMOD_RESULT ret = SO_CONTINUE(FMOD_RESULT, g_hook_fmod_cc_set_volume_cpp, channel_control, volume_raw);
    g_fmod_cc_set_volume_calls++;
    float volume = 0.0f;
    sceClibMemcpy(&volume, &volume_raw, sizeof(volume));
    if (g_fmod_cc_set_volume_calls <= 320 || volume < 0.01f || ret != 0) {
        l_audio("[AUDIO][FMODAPI] ChannelControl::setVolume#%u cc=%p raw=0x%08X vol=%f ret=%d",
                g_fmod_cc_set_volume_calls, channel_control, volume_raw, volume, ret);
    }
    return ret;
}
#endif

static uintptr_t install_fmod_hook(const char *symbol, void *hook_fn, so_hook *out) {
    uintptr_t addr = so_symbol(&fmod_mod, symbol);
    if (!addr) {
        l_audio("[AUDIO][FMODAPI] missing symbol %s", symbol);
        return 0;
    }
    *out = hook_addr(addr, (uintptr_t)hook_fn);
    return addr;
}

static void install_fmod_api_hooks(void) {
    uintptr_t addr_create_sound = install_fmod_hook("_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE",
                                                    (void *)&fmod_system_create_sound_cpp_hook,
                                                    &g_hook_fmod_sys_create_sound_cpp);
    if (!addr_create_sound) {
        l_warn("FMOD createSound hook not installed; BGM path compatibility fix is disabled.");
    }

#ifdef ENABLE_AUDIO_LOGS
    uintptr_t addr_sys_init = install_fmod_hook("_ZN4FMOD6System4initEijPv",
                                                (void *)&fmod_system_init_cpp_hook,
                                                &g_hook_fmod_sys_init);
    uintptr_t addr_set_output = install_fmod_hook("_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE",
                                                  (void *)&fmod_system_set_output_cpp_hook,
                                                  &g_hook_fmod_sys_set_output);
    uintptr_t addr_set_dsp = install_fmod_hook("_ZN4FMOD6System16setDSPBufferSizeEji",
                                               (void *)&fmod_system_set_dsp_buffer_cpp_hook,
                                               &g_hook_fmod_sys_set_dsp_buffer);
    uintptr_t addr_play_sound = install_fmod_hook("_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE",
                                                  (void *)&fmod_system_play_sound_cpp_hook,
                                                  &g_hook_fmod_sys_play_sound_cpp);
    uintptr_t addr_sound_release = install_fmod_hook("_ZN4FMOD5Sound7releaseEv",
                                                     (void *)&fmod_sound_release_cpp_hook,
                                                     &g_hook_fmod_sound_release_cpp);
    uintptr_t addr_set_mode = install_fmod_hook("_ZN4FMOD14ChannelControl7setModeEj",
                                                (void *)&fmod_channel_set_mode_cpp_hook,
                                                &g_hook_fmod_cc_set_mode_cpp);
    uintptr_t addr_set_paused = install_fmod_hook("_ZN4FMOD14ChannelControl9setPausedEb",
                                                  (void *)&fmod_channel_set_paused_cpp_hook,
                                                  &g_hook_fmod_cc_set_paused_cpp);
    uintptr_t addr_set_volume = install_fmod_hook("_ZN4FMOD14ChannelControl9setVolumeEf",
                                                  (void *)&fmod_channel_set_volume_cpp_hook,
                                                  &g_hook_fmod_cc_set_volume_cpp);

    so_flush_caches(&fmod_mod);
    l_audio("[AUDIO][FMODAPI] hooks installed sys_init=%p setOut=%p setDSP=%p createSound=%p playSound=%p release=%p setMode=%p setPaused=%p setVolume=%p",
            (void *)addr_sys_init, (void *)addr_set_output, (void *)addr_set_dsp,
            (void *)addr_create_sound, (void *)addr_play_sound,
            (void *)addr_sound_release, (void *)addr_set_mode,
            (void *)addr_set_paused, (void *)addr_set_volume);
#else
    so_flush_caches(&fmod_mod);
#endif
}

/*
 * Exit handler: ensure clean shutdown instead of crash when the game
 * tries to call System.exit() or similar.
 */
static void exit_process(void) {
    sceKernelExitProcess(0);
}

/*
 * FMOD guard patch:
 * Prevent a data-abort in libfmod when an invalid low address (e.g. 0x30)
 * is passed as ChannelControl handle to the internal getAsyncThread path.
 *
 * Original sequence at libfmod .text + 0x209cc:
 *   lsls r1, r0, #30
 *   bne  invalid_handle
 *
 * Patched sequence:
 *   lsrs r1, r0, #8
 *   beq  invalid_handle
 *
 * This rejects very low pointers before dereferencing [r0 + 0x148].
 */
static void patch_fmod_invalid_handle_guard(void) {
    if (!fmod_mod.text_base || fmod_mod.text_size < 0x209d0) {
        l_warn("FMOD guard patch skipped: invalid fmod_mod text range.");
        return;
    }

    uint16_t ins_lsrs_r1_r0_8 = 0x0A01; // lsrs r1, r0, #8
    uint16_t ins_beq_invalid = 0xD012;  // beq.n to existing invalid-handle return path

    uint32_t patch_addr_0 = fmod_mod.text_base + 0x209cc;
    uint32_t patch_addr_1 = fmod_mod.text_base + 0x209ce;

    kuKernelCpuUnrestrictedMemcpy((void *)patch_addr_0, &ins_lsrs_r1_r0_8, sizeof(ins_lsrs_r1_r0_8));
    kuKernelCpuUnrestrictedMemcpy((void *)patch_addr_1, &ins_beq_invalid, sizeof(ins_beq_invalid));

    so_flush_caches(&fmod_mod);
    l_info("Patched FMOD invalid-handle guard at +0x209cc/+0x209ce.");
}

void so_patch(void) {
    patch_fmod_invalid_handle_guard();
    install_fmod_api_hooks();

    /* Hook rateMeShown to prevent RateMe dialog crashes */
    uintptr_t addr = so_symbol(&so_mod, "_ZN3sys15AndroidPlatform11rateMeShownE");
    if (addr)
        hook_addr(addr, (uintptr_t)&ret0);

    /* Hook RateApp to prevent Google Play rating dialog */
    addr = so_symbol(&so_mod, "_ZN3sys15AndroidPlatform7RateAppEv");
    if (addr)
        hook_addr(addr, (uintptr_t)&ret0);

    /* Hide on-screen touch dpad and pause icon. cGameMain::Render draws the maze/HUD
     * via newPacman::LoopDraw, then — gated on [this+13] (!paused) — calls
     * cGUISystem::Render on its own widget system at [this+36], which is where the
     * touch dpad and pause button live. Forcing the gate to 0 skips only that branch
     * and leaves LoopDraw alone, so the HUD stays visible while the touch overlay
     * never gets drawn. The instruction at 0x164ed4 is `ldrb r1, [r0, #13]` (2 bytes,
     * 0x7b41); replace with `movs r1, #0` (0x2100) so r1 is always zero and the
     * following `beq` skips the cGUISystem::Render call unconditionally. */
    {
        uint32_t render_gate_addr = so_mod.text_base + 0x164ed4;
        uint16_t mov_r1_0 = 0x2100;
        kuKernelCpuUnrestrictedMemcpy((void *)render_gate_addr, &mov_r1_0, 2);
        l_info("Patched cGameMain::Render gate at 0x164ed4 -> movs r1, #0 (hides touch dpad).");
    }

    /* Also hide the in-game pause button drawn by pmcedx::GameScreen_Premium
     * (a different code path from the legacy cGameMain/cGUISystem dpad). */
    addr = so_symbol(&so_mod, "_ZN6pmcedx18GameScreen_Premium4InitEv");
    if (addr) {
        hook_addr(addr, (uintptr_t)&hide_pause_button_init);
        l_info("Hooked GameScreen_Premium::Init to hide pause button.");
    } else {
        l_warn("pmcedx::GameScreen_Premium::Init not found; pause button will remain.");
    }

    /* Stub out social/leaderboard network calls — no online services on Vita.
     * Use direct address patching since so_symbol may not find WEAK symbols. */
    struct { uint32_t offset; const char *name; } social_patches[] = {
        { 0x0013b02d, "Request::CreateSendOfflineScores" },
        { 0x0013b009, "Request::CreateGetTokensRequest" },
        { 0x0013b1c9, "SendScoreRequestData::~D2" },
        { 0x0013b175, "GetScoresRequestData::~D2" },
        { 0, NULL }
    };
    for (int i = 0; social_patches[i].name; i++) {
        /* Patch Thumb function: write "BX LR" (0x4770) at the entry point.
         * Addresses have bit 0 set for Thumb, clear it for the actual write. */
        uint32_t func_addr = so_mod.text_base + (social_patches[i].offset & ~1u);
        uint16_t bx_lr = 0x4770;
        kuKernelCpuUnrestrictedMemcpy((void *)func_addr, &bx_lr, 2);
        l_info("Patched %s at 0x%x", social_patches[i].name, social_patches[i].offset);
    }

    /* PGX1 sidecar loader: hooks png_read_image so the per-file RGBA buffer
     * in foo.png.gxt (emitted by the website packager) bypasses libpng's
     * per-pixel CPU decode. Expected win: ~10–15 s of startup → ~1–2 s. */
    pgxt_install_hooks();
    so_flush_caches(&so_mod);

    l_info("Patches applied.");
}
