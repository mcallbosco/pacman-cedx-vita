#include "game_audio.h"
#include "game_patch.h"
#include <stdbool.h>
#include <string.h>

extern so_module fmod_mod;

enum { POWER_CUE = 52, LOOP_NORMAL = 2, RETRY_FRAMES = 60 };

static so_hook destroy_hook, sfx_hook;
static uintptr_t bundler_resume __attribute__((used));
static void **enemy_root, **preferences;
static const bool *pause_menu_shown;
static bool (*is_preview)(void), (*is_paused)(void), (*is_tutorial_paused)(void);
static bool (*is_pref)(void *, const char *);
static bool (*sfx_valid)(void *);
static void (*play_cue)(void *, int, bool);
static int (*play_sound)(void *, void *, void *, bool, void **);
static int (*system_update)(void *);
static int (*channel_stop)(void *);
static int (*channel_playing)(void *, bool *);
static int (*channel_mode)(void *, unsigned);
static int (*channel_volume)(void *, float);
static int (*channel_paused)(void *, bool);

static void *owner, *loop_channel;
static bool wanted, starting_loop;
static unsigned retry_frames;

#ifdef ENABLE_AUDIO_LOGS
static void **sound2_instance;
static void *music_sound, *music_channel;
static unsigned diagnostic_frames;
static int (*channels_playing)(void *, int *, int *);
static int (*sound_open_state)(void *, int *, unsigned *, bool *, bool *);
static int (*get_channel_paused)(void *, bool *);
static int (*get_channel_mode)(void *, unsigned *);
static int (*get_channel_position)(void *, unsigned *, unsigned);
#endif

static uint32_t word(const void *object, size_t offset) {
    uint32_t value;
    memcpy(&value, (const char *)object + offset, sizeof(value));
    return value;
}

#ifdef ENABLE_AUDIO_LOGS
void game_audio_log_play(void *sound, void *group, int result, void *channel) {
    void *sound2 = sound2_instance ? *sound2_instance : NULL;
    if (!sound2 || group != (void *)(uintptr_t)word(sound2, 12))
        return;
    l_audio("[MUSIC] play cue=%u sound=%p group=%p channel=%p result=%d",
            word(sound2, 20), sound, group, channel, result);
    if (result == 0) {
        music_sound = sound;
        music_channel = channel;
        diagnostic_frames = 59;
    }
}

void game_audio_log_release(void *sound) {
    if (sound == music_sound) {
        l_audio("[MUSIC] release sound=%p channel=%p", sound, music_channel);
        music_sound = NULL;
        music_channel = NULL;
    }
}

static void log_music_state(void *sound2, int update_result) {
    if (++diagnostic_frames < 60)
        return;
    diagnostic_frames = 0;
    if (!music_channel || !channels_playing || !sound_open_state ||
        !get_channel_paused || !get_channel_mode || !get_channel_position)
        return;
    int total = -1, real = -1, open = -1;
    unsigned buffered = 0, mode = 0, position = 0;
    bool playing = false, paused = false, starving = false, busy = false;
    int count_result = channels_playing((void *)(uintptr_t)word(sound2, 4), &total, &real);
    int play_result = channel_playing(music_channel, &playing);
    int pause_result = get_channel_paused(music_channel, &paused);
    int mode_result = get_channel_mode(music_channel, &mode);
    int position_result = get_channel_position(music_channel, &position, 1); /* milliseconds */
    int open_result = sound_open_state(music_sound, &open, &buffered, &starving, &busy);
    l_audio("[MUSIC] state cue=%u channel=%p playing=%d/%d paused=%d/%d mode=%x/%d ms=%u/%d open=%d/%d buffer=%u starving=%d busy=%d voices=%d/%d/%d update=%d power=%p",
            word(sound2, 20), music_channel, playing, play_result, paused, pause_result,
            mode, mode_result, position, position_result, open, open_result, buffered,
            starving, busy, total, real, count_result, update_result, loop_channel);
}
#endif

static void stop_channel(void) {
    if (loop_channel) {
        /* FMOD handles can become invalid after a group stop or voice theft.
         * Forget the handle even if stop reports that it is already gone. */
        int result = channel_stop(loop_channel);
        l_audio("[POWER] stop channel=%p result=%d", loop_channel, result);
        (void)result;
        loop_channel = NULL;
    }
    retry_frames = 0;
}

static void stop_wave(void *sound_manager) {
    (void)sound_manager;
    wanted = false;
    stop_channel();
}

/* Replay the checked entry once, then resume before its first PC-relative
 * load. The native epilogue unwinds this frame; the entry stays hooked. */
static void __attribute__((naked, noinline)) bundler_original(void *self) {
    __asm__(
        "push {r7, lr}\n"
        "mov r7, sp\n"
        "sub sp, #80\n"
        "mov r1, r0\n"
        "ldr ip, =bundler_resume\n"
        "ldr ip, [ip]\n"
        "bx ip\n");
}

static void bundler_update(void *self) {
    bundler_original(self);

    /* Keep the native cue selection, including power expiry, death and the
     * bundler's sound-disable flag. Other character roots do not own audio.
     * Android updates this field but omitted the corresponding PlayWave. */
    if (self == *enemy_root) {
        if (owner != self)
            stop_channel();
        owner = self;
        wanted = word(self, 0x30) == POWER_CUE &&
                 !((const unsigned char *)self)[0x34];
        if (!wanted)
            stop_channel();
    }
}

static void *bundler_destroy(void *self) {
    if (self == owner) {
        stop_wave(NULL);
        owner = NULL;
    }
    return SO_CONTINUE(void *, destroy_hook, self);
}

static void sfx_play(void *self, void *system, void *group, int priority, bool loop) {
    if (!starting_loop) {
        so_hook_unpatch(&sfx_hook);
        ((void (*)(void *, void *, void *, int, bool))sfx_hook.thumb_addr)(
            self, system, group, priority, loop);
        so_hook_repatch(&sfx_hook);
        return;
    }

    /* Only our synchronous PlaySound2(ijike) call takes this path. Resource
     * loading, the sound preference and SFX group selection remain native.
     * Retain our own channel; the original FmodSfx::Play loses its handle. */
    if (!sfx_valid(self))
        return;
    void *channel = NULL;
    void *sound = (void *)(uintptr_t)word(self, 12);
    float volume;
    memcpy(&volume, (const char *)self + 16, sizeof(volume));
    int result = play_sound(system, sound, group, true, &channel);
    l_audio("[POWER] play sound=%p channel=%p result=%d", sound, channel, result);
    if (result != 0 || !channel)
        return;
    /* Configure before unpausing, so the mixer never sees a one-shot at the
     * wrong volume. Do not change the shared FMOD::Sound's loop mode. */
    if (channel_volume(channel, volume) != 0 ||
        channel_mode(channel, LOOP_NORMAL) != 0 ||
        channel_paused(channel, false) != 0) {
        channel_stop(channel);
        return;
    }
    loop_channel = channel;
}

static void update_loop(void *sound2) {
    /* Do not dereference owner here: a scene may have replaced its task tree
     * since the last gameplay tick. The destructor also clears ownership. */
    if (!owner || owner != *enemy_root || !wanted || *pause_menu_shown ||
        is_paused() || is_tutorial_paused() || is_preview() ||
        !*preferences || !is_pref(*preferences, "sound")) {
        stop_channel();
        return;
    }
    if (loop_channel) {
        bool playing = false;
        int result = channel_playing(loop_channel, &playing);
        if (result == 0 && playing)
            return;
        l_audio("[POWER] lost channel=%p playing=%d result=%d", loop_channel, playing, result);
        loop_channel = NULL;
    }
    if (retry_frames) {
        --retry_frames;
        return;
    }
    starting_loop = true;
    /* Intercept only this dispatch, avoiding a hook/unhook and cache flush
     * for every ordinary pellet/ghost sound during gameplay. */
    so_hook_repatch(&sfx_hook);
    play_cue(sound2, POWER_CUE, true);
    so_hook_unpatch(&sfx_hook);
    starting_loop = false;
    /* Missing/unloadable assets must not cause disk retries every frame. */
    retry_frames = loop_channel ? 0 : RETRY_FRAMES;
}

static void sound_update(void *self) {
    update_loop(self);
    /* The checked Sound2::Update body only calls System::update on +4. */
    int result = system_update((void *)(uintptr_t)word(self, 4));
#ifdef ENABLE_AUDIO_LOGS
    log_music_state(self, result);
#else
    (void)result;
#endif
}

void game_audio_install_hooks(void) {
#ifdef ENABLE_AUDIO_LOGS
    sound2_instance = (void *)so_symbol(&so_mod,
        "_ZN3sys9SingletonINS_6Sound2EE11s_pInstanceE");
    channels_playing = (void *)so_symbol(&fmod_mod, "_ZN4FMOD6System18getChannelsPlayingEPiS1_");
    sound_open_state = (void *)so_symbol(&fmod_mod, "_ZN4FMOD5Sound12getOpenStateEP14FMOD_OPENSTATEPjPbS4_");
    get_channel_paused = (void *)so_symbol(&fmod_mod, "_ZN4FMOD14ChannelControl9getPausedEPb");
    get_channel_mode = (void *)so_symbol(&fmod_mod, "_ZN4FMOD14ChannelControl7getModeEPj");
    get_channel_position = (void *)so_symbol(&fmod_mod, "_ZN4FMOD7Channel11getPositionEPjj");
    l_audio("[MUSIC] diagnostic build: power-loop restoration active");
#endif
    uintptr_t bundler = game_patch_checked_function(
        "_ZN9newPacman27cOnCharactorKindBundlerTask4FuncEv", 816, 0xce77a525u);
    uintptr_t destroy = game_patch_checked_function(
        "_ZN9newPacman27cOnCharactorKindBundlerTaskD1Ev", 26, 0x303e9638u);
    uintptr_t update = game_patch_checked_function(
        "_ZN3sys6Sound26UpdateEv", 26, 0x664736d1u);
    uintptr_t sfx = game_patch_checked_function(
        "_ZN3sys7FmodSfx4PlayEPN4FMOD6SystemEPNS1_12ChannelGroupEib", 200, 0x3d527969u);
    uintptr_t stop = game_patch_checked_function(
        "_ZN3sys12SoundManager8StopWaveEv", 12, 0x87cf7ea5u);
    play_cue = (void *)game_patch_checked_function(
        "_ZN3sys6Sound210PlaySound2Eib", 232, 0xb5f7c67eu);
    sfx_valid = (void *)game_patch_checked_function(
        "_ZN3sys7FmodSfx7IsValidEv", 48, 0x4b74106bu);
    is_preview = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence9IsPreviewEv", 20, 0x1128e4ffu);
    is_paused = (void *)game_patch_checked_function(
        "_ZN9newPacman12GetPauseFlagEv", 16, 0xd3471ef9u);
    is_tutorial_paused = (void *)game_patch_checked_function(
        "_ZN9newPacman20GetTutorialPauseFlagEv", 16, 0x01479782u);
    is_pref = (void *)so_symbol(&so_mod, "_ZN6pmcedx11Preferences6IsPrefEPKc");
    preferences = (void *)so_symbol(&so_mod,
        "_ZN3sys9SingletonIN6pmcedx11PreferencesEE11s_pInstanceE");
    enemy_root = (void *)so_symbol(&so_mod,
        "_ZN9newPacman27cOnCharactorKindBundlerTask10pEnemyRootE");
    pause_menu_shown = (void *)so_symbol(&so_mod,
        "_ZN6pmcedx23PauseMenuScreen_Premium13s_bPauseShownE");
    play_sound = (void *)so_symbol(&fmod_mod,
        "_ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE");
    system_update = (void *)so_symbol(&fmod_mod, "_ZN4FMOD6System6updateEv");
    channel_stop = (void *)so_symbol(&fmod_mod, "_ZN4FMOD14ChannelControl4stopEv");
    channel_playing = (void *)so_symbol(&fmod_mod, "_ZN4FMOD14ChannelControl9isPlayingEPb");
    channel_mode = (void *)so_symbol(&fmod_mod, "_ZN4FMOD14ChannelControl7setModeEj");
    channel_volume = (void *)so_symbol(&fmod_mod, "_ZN4FMOD14ChannelControl9setVolumeEf");
    channel_paused = (void *)so_symbol(&fmod_mod, "_ZN4FMOD14ChannelControl9setPausedEb");

    /* All-or-nothing: never start an indefinite loop without its stop path. */
    if (!bundler || !destroy || !update || !sfx || !stop || !play_cue ||
        !sfx_valid || !is_preview || !is_paused || !is_tutorial_paused ||
        !is_pref || !preferences || !enemy_root || !pause_menu_shown ||
        !play_sound || !system_update || !channel_stop || !channel_playing || !channel_mode ||
        !channel_volume || !channel_paused) {
        l_warn("Power-pellet audio patch skipped: unsupported sound code.");
        return;
    }
    bundler_resume = bundler + 8;
    hook_addr(bundler, (uintptr_t)bundler_update);
    destroy_hook = hook_addr(destroy, (uintptr_t)bundler_destroy);
    hook_addr(update, (uintptr_t)sound_update);
    sfx_hook = hook_addr(sfx, (uintptr_t)sfx_play);
    so_hook_unpatch(&sfx_hook);
    hook_addr(stop, (uintptr_t)stop_wave);
    l_info("Restored power-pellet sound loop.");
}
