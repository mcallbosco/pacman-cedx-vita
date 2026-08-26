#include "game_maze_preload.h"
#include "game_patch.h"

#include <psp2/kernel/threadmgr.h>
#include <string.h>
#include <vitaGL.h>

static uintptr_t maze_ctor_resume __attribute__((used));
static uintptr_t main_ctor_resume __attribute__((used));
static uintptr_t jingle_resume __attribute__((used));
static void *pending_jingle __attribute__((used));
static void (*maze_play_cue)(void *, int, int);
static int (*maze_is_game)(void);
static int (*maze_is_preview)(void);
static int (*maze_texture_id)(void *, int, int);
static void *(*maze_resources)(void);
static void *(*maze_importer)(void);
static void *(*maze_texture_manager)(void);
static const char *(*maze_skin)(void *);
static int (*maze_loading)(void *);
static int (*maze_reloading)(void *);
static void *(*maze_resource_data)(void *, int, void *, const char *, int, int);
static int (*maze_legal)(void *);
static void (*maze_generate)(void *, int);
static uint64_t (*maze_milliseconds)(void);
static uint64_t *maze_last_tick;
static const unsigned *maze_sequence_size;

/* Match the renderer destructor's per-run unload range. All supplied courses
 * use this range; the unused maze71 resource is outside that lifetime. */
enum { FIRST_MAZE_TEXTURE = 27, LAST_MAZE_TEXTURE = 103, MAX_COURSE_PHASES = 200 };

static uint32_t texture_word(const void *texture, unsigned offset) {
    uint32_t value;
    memcpy(&value, (const char *)texture + offset, sizeof(value));
    return value;
}

static void preload_course(void) {
    /* Previews also create this renderer. Only warm a real selected run,
     * after LoadInitialMapTextures has finished unloading the old resources. */
    if (!maze_is_game() || maze_is_preview())
        return;
    unsigned phases = *maze_sequence_size;
    if (!phases || phases > MAX_COURSE_PHASES)
        return;

    void *resources = maze_resources();
    void *importer = maze_importer();
    void *manager = maze_texture_manager();
    if (!resources || !importer || !manager)
        return;
    const char *skin = maze_skin(manager);
    if (!skin || !*skin)
        return;

    uint64_t start = maze_milliseconds();
    /* Both workers can mutate texture objects. Let them finish before the
     * main thread imports anything. A stuck worker retains the native lazy
     * path instead of indefinitely blocking course startup. */
    while (maze_loading(resources) || maze_reloading(resources)) {
        if (maze_milliseconds() - start >= 5000)
            goto finished;
        sceKernelDelayThread(1000);
    }

    GLint binding;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    unsigned char seen[LAST_MAZE_TEXTURE - FIRST_MAZE_TEXTURE + 1] = {0};
    for (unsigned phase = 0; phase < phases; ++phase) {
        for (int side = 0; side <= 2; side += 2) {
            /* This native helper does not use its instance. Preserve its
             * course/map/texture aliases, including left/right bank mapping. */
            int id = maze_texture_id(NULL, (int)phase, side);
            if (id < FIRST_MAZE_TEXTURE || id > LAST_MAZE_TEXTURE ||
                seen[id - FIRST_MAZE_TEXTURE])
                continue;
            seen[id - FIRST_MAZE_TEXTURE] = 1;

            /* Decode and upload one at a time. Cache hits may still contain
             * only CPU pixels from the async loader, so explicitly generate
             * those too. Generate(false) frees each decoded buffer and runs
             * the existing Vita atlas conversion before the next texture. */
            void *texture = maze_resource_data(resources, id, importer, skin, 1, 1);
            if (texture && !texture_word(texture, 32) &&
                texture_word(texture, 44) && maze_legal(texture))
                maze_generate(texture, 0);
        }
    }
    /* GenerateTexture only changes the binding on the current texture unit.
     * Keep the renderer's cached binding consistent with GL after warmup. */
    glBindTexture(GL_TEXTURE_2D, (GLuint)binding);

finished:
    /* Countdown/gameplay tasks are frame based. App delta time uses a wall
     * clock: exclude this synchronous wait from its following update too. */
    *maze_last_tick += maze_milliseconds() - start;
}

static void * __attribute__((naked)) original_maze_ctor(void *self, void *sprite, int kind) {
    __asm__ volatile(
        "push {r7, lr}\n"
        "mov r7, sp\n"
        "sub sp, sp, #96\n"
        "mov r3, r2\n"
        "ldr r12, =maze_ctor_resume\n"
        "ldr r12, [r12]\n"
        "bx r12\n"
        ".ltorg\n");
}

static void *maze_ctor(void *self, void *sprite, int kind) {
    void *result = original_maze_ctor(self, sprite, kind);
    preload_course();
    void *sound = pending_jingle;
    pending_jingle = NULL;
    if (sound && maze_is_game() && !maze_is_preview())
        maze_play_cue(sound, 8, 0);
    return result;
}

/* Only the initial-character-task call for cue 8 is deferred. Its frame-based
 * countdown stays frozen during warmup, so the sound must start afterward. */
static void __attribute__((used, noinline)) defer_start_jingle(void *sound) {
    if (maze_is_game() && !maze_is_preview())
        pending_jingle = sound;
    else
        maze_play_cue(sound, 8, 0);
}

static void __attribute__((naked)) jingle_bridge(void) {
    __asm__ volatile(
        "push {r4, lr}\n"
        "bl defer_start_jingle\n"
        "pop {r4, lr}\n"
        "ldr r12, =jingle_resume\n"
        "ldr r12, [r12]\n"
        "bx r12\n"
        ".ltorg\n");
}

/* Every run/retry and rebuilt preview gets a new main task before its maze
 * renderer. Clear any cue left by a cancelled start before that task exists. */
static void __attribute__((naked)) main_ctor_bridge(void) {
    __asm__ volatile(
        "ldr r12, =pending_jingle\n"
        "movs r3, #0\n"
        "str r3, [r12]\n"
        "push {r7, lr}\n"
        "mov r7, sp\n"
        "sub sp, sp, #40\n"
        "mov r3, r2\n"
        "ldr r12, =main_ctor_resume\n"
        "ldr r12, [r12]\n"
        "bx r12\n"
        ".ltorg\n");
}

void game_maze_preload_install_hooks(void) {
    uintptr_t ctor = game_patch_checked_function(
        "_ZN9newPacman14cNewGenMapNeonC2EPN3sys9cRTSpriteEi", 0x180, 0x1fb229aeu);
    uintptr_t main_ctor = game_patch_checked_function(
        "_ZN9newPacman11cTsTaskMainC2EiNS0_11PreviewTypeE", 0x84, 0xcc135ac3u);
    uintptr_t initial_characters = game_patch_checked_function(
        "_ZN9newPacman11CPacmanGame19FuncInitialCharTaskEv", 0x2b4, 0x86a59f91u);
    maze_play_cue = (void *)game_patch_checked_function(
        "_ZN3sys6Sound210PlaySound2Eib", 232, 0xb5f7c67eu);
    maze_is_game = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence6IsGameEv", 20, 0x476f8a74u);
    maze_is_preview = (void *)game_patch_checked_function(
        "_ZN9newPacman15cTsTaskSequence9IsPreviewEv", 20, 0x1128e4ffu);
    maze_texture_id = (void *)game_patch_checked_function(
        "_ZN9newPacman19AsyncTexturesLoader12GetTextureIdEii", 114, 0x6261b6bfu);
    maze_resources = (void *)game_patch_checked_function(
        "_ZN3sys9SingletonINS_10ResManagerEE11GetInstanceEv", 72, 0xfdcf0612u);
    maze_importer = (void *)game_patch_checked_function(
        "_ZN3sys9SingletonINS_15TextureImporterEE11GetInstanceEv", 72, 0x85f645beu);
    maze_texture_manager = (void *)game_patch_checked_function(
        "_ZN3sys7cCommon17TextureManagerGetEv", 16, 0x0585419au);
    maze_skin = (void *)game_patch_checked_function(
        "_ZN3sys14TextureManager17GetCurMapSkinInfoEv", 26, 0x4a46596bu);
    maze_loading = (void *)game_patch_checked_function(
        "_ZN3sys10ResManager9IsLoadingEv", 24, 0xc481aa63u);
    maze_reloading = (void *)game_patch_checked_function(
        "_ZN3sys10ResManager24IsReloadingAsyncTexturesEv", 24, 0x7d14d38bu);
    maze_resource_data = (void *)game_patch_checked_function(
        "_ZN3sys10ResManager15GetResourceDataEiPNS_12DataImporterEPKcbb", 564, 0x762aba87u);
    maze_legal = (void *)game_patch_checked_function(
        "_ZN3sys7ResData7IsLegalEv", 20, 0x46dca993u);
    maze_generate = (void *)game_patch_checked_function(
        "_ZN3sys7Texture8GenerateEb", 72, 0xfce90bfdu);
    maze_milliseconds = (void *)game_patch_checked_function(
        "_ZN3sys19GetTimeMilliSecondsEv", 104, 0x30ee940fu);
    maze_last_tick = (void *)so_symbol(&so_mod, "_ZN3sys3App11s_nLastTickE");
    maze_sequence_size = (void *)so_symbol(&so_mod,
        "_ZN9newPacman13cOnCourceData17m_uiCourseSeqSizeE");

    /* These unmodified helpers define the native course-to-resource mapping. */
    uintptr_t map_id = game_patch_checked_function(
        "_ZN9newPacman13cOnCourceData8GetMapIDEjj", 104, 0x35528279u);
    uintptr_t tex_id = game_patch_checked_function(
        "_ZN9newPacman10cMapBuffer8GetTexIDEii", 280, 0xa6a204d2u);
    uintptr_t map_texture = game_patch_checked_function(
        "_ZN9newPacman14cTsTaskMapCtrl13GetMapTextureEii", 42, 0x7582cdabu);
    uintptr_t map_mmg = game_patch_checked_function(
        "_ZN9newPacman14cTsTaskMapCtrl9GetMapMmgEii", 88, 0x66f74d0cu);
    if (!ctor || !main_ctor || !initial_characters || !maze_play_cue ||
        !maze_is_game || !maze_is_preview || !maze_texture_id ||
        !maze_resources || !maze_importer || !maze_texture_manager || !maze_skin ||
        !maze_loading || !maze_reloading || !maze_resource_data || !maze_legal ||
        !maze_generate || !maze_milliseconds || !maze_last_tick || !maze_sequence_size ||
        !map_id || !tex_id || !map_texture || !map_mmg)
        return;
    maze_ctor_resume = ctor + 8;
    main_ctor_resume = main_ctor + 8;
    jingle_resume = initial_characters + 0x1dc;
    hook_addr(ctor, (uintptr_t)maze_ctor);
    hook_addr(main_ctor, (uintptr_t)main_ctor_bridge);
    hook_addr(initial_characters + 0x1d4, (uintptr_t)jingle_bridge);
}
