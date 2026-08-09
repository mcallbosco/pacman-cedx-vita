#include "game_speed.h"
#include "game_patch.h"
#include "settings.h"
#include <string.h>

static void *(*search_param_node)(const char *, int);

/* CommonFile.csv in the matching PC gamedata_iphone.bin pack. Only column
 * zero differs from Android; ghost-relative factors and course progression
 * remain native. Exact values avoid rounding/compounding a 5/3 multiplier. */
static const float pc_speed_ranks[50] = {
    0.40f, 0.50f, 0.60f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f,
    1.02f, 1.04f, 1.06f, 1.08f, 1.10f, 1.12f, 1.14f, 1.16f, 1.18f, 1.20f,
    1.22f, 1.24f, 1.26f, 1.28f, 1.30f, 1.32f, 1.34f, 1.36f, 1.38f, 1.40f,
    1.42f, 1.44f, 1.46f, 1.48f, 1.50f, 1.52f, 1.54f, 1.56f, 1.58f, 1.60f,
    1.62f, 1.64f, 1.66f, 1.68f, 1.70f, 1.72f, 1.74f, 1.76f, 1.78f, 1.80f,
};

static float speed_lookup(const char *key, float fallback, int column) {
    if (setting_pcSpeed && column == 0 && key &&
        strncmp(key, "speedrank", 9) == 0) {
        const char *digits = key + 9;
        if (digits[0] >= '1' && digits[0] <= '9') {
            unsigned rank = digits[0] - '0';
            ++digits;
            if (*digits >= '0' && *digits <= '9') {
                rank = rank * 10 + *digits - '0';
                ++digits;
            }
            if (*digits == '\0' && rank <= 50)
                return pc_speed_ranks[rank - 1];
        }
    }
    /* The checked native lookup searches both parameter packs, then reads
     * float[column] at node+64.*/
    void *node = search_param_node(key, -1);
    if (!node)
        return fallback;
    float result;
    memcpy(&result, (const char *)node + 64 + column * sizeof(float), sizeof(result));
    return result;
}

void game_speed_install_hooks(void) {
    if (!setting_pcSpeed)
        return;
    uintptr_t lookup = game_patch_checked_function(
        "_ZN9newPacman15cParamFileChunk16SearchParamFloatEPKcfi", 80, 0xf517a8e2u);
    uintptr_t rank = game_patch_checked_function(
        "_ZN9newPacman11CPacmanGame12SetSpeedRankEi", 1164, 0x35bcbdfdu);
    search_param_node = (void *)so_symbol(&so_mod,
        "_ZN9newPacman15cParamFileChunk15SearchParamNodeEPKci");
    if (lookup && rank && search_param_node)
        hook_addr(lookup, (uintptr_t)speed_lookup);
}
