#include "game_rules.h"
#include "game_rules_data.h"
#include "game_patch.h"
#include "settings.h"
#include <stdlib.h>
#include <string.h>

static so_hook dataset_hook;
static void *(*search_node)(const char *, int);
static void **game_model_instance;
static void *(*selected_course)(void *);

#include "game_rules_text.h"

static const char *text_lookup(void *self, int id) {
    const char **strings;
    memcpy(&strings, (const char *)self + 12, sizeof(strings));
    if (id == 21 || id == 79)
        return strings[id + 1];
    const char *text = strings[id];
    if ((id == 22 || id == 80) && text) {
        for (size_t i = 0; i < sizeof(pc_timer_text) / sizeof(pc_timer_text[0]); ++i) {
            if (pc_timer_text[i].id == id && strcmp(text, pc_timer_text[i].android) == 0)
                return pc_timer_text[i].pc;
        }
    }
    return text;
}

static void dataset(void *self, void *data, unsigned size) {
    size_t length = 0;
    char *patched = game_rules_transform(data, size, &length);
    /* Loading-time replacement lets the native parser populate its base data,
     * timer conversions and course copies consistently. Training rows and
     * Android-only metadata pass through unchanged. */
    if (!patched) l_warn("PC course rules: could not transform stage data");
    so_hook_unpatch(&dataset_hook);
    uintptr_t original = dataset_hook.thumb_addr ? dataset_hook.thumb_addr : dataset_hook.addr;
    ((void (*)(void *, void *, unsigned))original)(self, patched ? patched : data,
                                                 patched ? (unsigned)length : size);
    so_hook_repatch(&dataset_hook);
    free(patched);
}

static int integer_lookup(const char *key, int fallback, int column) {
    /* Android SPFile shadows the matching CommonFile value. PC SPFile does not
     * contain this key; PC's lookup at 0x4e9745 resolves to 250000. */
    if (key && strcmp(key, "addZankiScore") == 0 && column == 0)
        return 250000;
    void *node = search_node(key, -1);
    if (!node) return fallback;
    int result;
    memcpy(&result, (const char *)node + 72 + column * sizeof(int), sizeof(result));
    return result;
}

static int fixed_difficulty(const void *course) {
    if (!course) return 0;
    uint16_t id, map;
    uint32_t seconds;
    memcpy(&id, (const char *)course + 0x44, sizeof(id));
    memcpy(&map, (const char *)course + 0x48, sizeof(map));
    memcpy(&seconds, (const char *)course + 0x50, sizeof(seconds));
    /* The timer guard leaves the original course alone if CSV import failed. */
    return seconds == 600 && game_rules_fixed_difficulty(id, map);
}

static void set_difficulty(void *course, int difficulty) {
    /* PC's menu only exposes slot zero for ten-minute Score Attacks. Keep
     * subsequent selection changes and retries out of its unused slots. */
    if (fixed_difficulty(course)) difficulty = 0;
    void *saved;
    memcpy(&saved, (const char *)course + 0xf0, sizeof(saved));
    memcpy((char *)saved + 0x3c, &difficulty, sizeof(difficulty));
}

static uint8_t *settings_config(void *configs, int mode) {
    /* Checked SLSConfig vector accessor, used only by the native settings
     * setup. Change its temporary Score Attack row; native layout and input
     * navigation already omit hidden selectors. Other visibility flags stay
     * intact, and the course's actual game-mode identifier does not change. */
    uint8_t *begin;
    memcpy(&begin, configs, sizeof(begin));
    uint8_t *config = begin + mode * 6;
    if (mode == 0 && *game_model_instance) {
        void *course = selected_course(*game_model_instance);
        if (fixed_difficulty(course)) {
            config[0] = 0;
            /* ShowAlternative skips its callback if index zero is already
             * selected. Normalize the saved value even in that case. */
            set_difficulty(course, 0);
        }
    }
    return config;
}

void game_rules_install_hooks(void) {
    if (!setting_pcRules) return;
    uintptr_t load = game_patch_checked_function(
        "_ZN9newPacman17cGameScoreManager10DataSetNewEPvj", 2504, 0x4091655du);
    uintptr_t integers = game_patch_checked_function(
        "_ZN9newPacman15cParamFileChunk14SearchParamIntEPKcii", 74, 0x603f7a5du);
    uintptr_t text = game_patch_checked_function(
        "_ZN3sys8TextPack9GetStringEi", 28, 0xa0ecb10eu);
    uintptr_t difficulty = game_patch_checked_function(
        "_ZN9newPacman12cCourseParam12SetHardLevelEi", 28, 0xcfc12ef3u);
    uintptr_t setup = game_patch_checked_function(
        "_ZN6pmcedx33SelectLevelSettingsScreen_Premium5SetupEii", 912, 0xe65de760u);
    uintptr_t config = setup ? game_patch_checked_code(
        setup + 0x4ec, "SLSConfig vector accessor", 32, 0x882c0561u) : 0;
    selected_course = (void *)game_patch_checked_function(
        "_ZN6pmcedx9GameModel27GetSelectedLevelCourseParamEv", 30, 0xfbfc31fdu);
    game_model_instance = (void **)so_symbol(&so_mod,
        "_ZN3sys9SingletonIN6pmcedx9GameModelEE11s_pInstanceE");
    search_node = (void *)so_symbol(&so_mod,
        "_ZN9newPacman15cParamFileChunk15SearchParamNodeEPKci");
    if (!load || !integers || !text || !difficulty || !config ||
        !selected_course || !game_model_instance || !search_node) return;
    dataset_hook = hook_addr(load, (uintptr_t)dataset);
    hook_addr(integers, (uintptr_t)integer_lookup);
    hook_addr(text, (uintptr_t)text_lookup);
    hook_addr(difficulty, (uintptr_t)set_difficulty);
    hook_addr(config, (uintptr_t)settings_config);
}
