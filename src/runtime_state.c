/* Where the player is: the screen or the mode, for the log's [state] lines and the Discord status
 * (runtime_discord.c). The game says it through the files it opens for each screen (runtime_file.c
 * hands every opened game file to runtime_state_file):
 * - the title opens mn_title_<lang>.pac, the main menu mn_main_<lang>.pac (again each time it comes
 *   back), Settings mn_option.pac;
 * - a mode opens its own menu: mn_camp (God of Destruction and King of Kaiju alike), mn_vs, mn_evol,
 *   diorama_<LANG>, mn_extra; a stage its map (map<area><stage>.pac: map51 is the 1954 tutorial) and its
 *   kaiju (ch<NN>a_00.pac).
 * Which mode: the main menu stores the item chosen in a global before the mode's menu loads (0x650040
 * in 01.01, 01.00 and the European 01.00; written at 0x2c783 in 01.01): 0 God of Destruction, 1 King
 * of Kaiju, 2 VS, 3 Evolution, 4 Diorama, 5 Kaiju Guide. It keeps that value back in the main menu,
 * which the files tell. A new game started from the title (the tutorial) has not set it: 0. */
#include "runtime.h"
#include "signature.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* The main menu's store of the item chosen: lea rcx, [rip + mode]; mov [rcx], eax; then it clears two
 * more globals and loads the save data (rcx in 01.01, rdx in 01.00) and the menu's flags. */
static const MtSignature mode_store = {
    "48 8d 0d ?? ?? ?? ?? 89 01 48 8d 05 ?? ?? ?? ?? c7 00 00 00 00 00 48 8d 0d ?? ?? ?? ?? c7 01 00 00 00 00 "
    "48 8d ?? ?? ?? ?? ?? 8b ?? d0 01 00 00", 3};

static const char *const mode_names[] = {"God of Destruction Mode", "King of Kaiju Mode", "VS Mode",
                                         "Evolution Mode", "Diorama Mode", "Kaiju Guide"};
enum { MODE_COUNT = sizeof(mode_names) / sizeof(*mode_names) };

static const volatile int32_t *mode_global;
static int scene = RUNTIME_SCENE_STARTING, mode = -1, map = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void runtime_state_configure(unsigned char *image, uint64_t size) {
    const uint64_t at = mt_signature_find(image, size, &mode_store);
    mode_global = NULL;
    if (at != UINT64_MAX) {
        int32_t displacement;
        memcpy(&displacement, image + at, 4);
        const uint64_t global = at + 4 + (uint64_t)(int64_t)displacement;
        if (global + 4 <= size) mode_global = (const volatile int32_t *)(image + global);
    }
    if (mode_global) printf("[state] the main menu's mode at image offset 0x%" PRIx64 "\n",
                            (uint64_t)((const unsigned char *)mode_global - image));
    else puts("[state] the main menu's mode was not found in this game build: the modes are not told apart");
}

/* The screen a game file opens (RUNTIME_SCENE_*), or -1 for a file that says nothing; *stage: the map's
 * number for a stage map (map32.pac: 32), else -1. */
int runtime_state_scene_of(const char *guest, int *stage) {
    const char *name = strrchr(guest, '/');
    name = name ? name + 1 : guest;
    *stage = -1;
    if (!strncmp(name, "mn_title", 8)) return RUNTIME_SCENE_TITLE;
    if (!strncmp(name, "mn_main", 7)) return RUNTIME_SCENE_MAIN_MENU;
    if (!strncmp(name, "mn_option", 9)) return RUNTIME_SCENE_SETTINGS;
    if (!strncmp(name, "mn_camp", 7) || !strncmp(name, "mn_vs", 5) || !strncmp(name, "mn_evol", 7) ||
        !strncmp(name, "mn_extra", 8) || !strncmp(name, "diorama_", 8))
        return RUNTIME_SCENE_MODE;
    if (!strncmp(name, "map", 3) && name[3] >= '0' && name[3] <= '9' && name[4] >= '0' && name[4] <= '9' &&
        !strcmp(name + 5, ".pac")) {
        *stage = (name[3] - '0') * 10 + (name[4] - '0');
        return RUNTIME_SCENE_MODE;
    }
    return -1;
}

/* The text for a screen and mode (the Discord status): "Main menu", "King of Kaiju Mode". */
const char *runtime_state_name(int which, int mode_index, int online) {
    switch (which) {
    case RUNTIME_SCENE_TITLE: return "Title screen";
    case RUNTIME_SCENE_MAIN_MENU: return "Main menu";
    case RUNTIME_SCENE_SETTINGS: return "Settings";
    case RUNTIME_SCENE_MODE:
        if (mode_index == 2 && online) return "VS Mode (online)";
        return mode_index >= 0 && mode_index < MODE_COUNT ? mode_names[mode_index] : "Playing";
    default: return "Starting the game";
    }
}

void runtime_state_file(const char *guest) {
    int stage;
    const int which = runtime_state_scene_of(guest, &stage);
    if (which < 0) return;
    const int chosen = mode_global ? *mode_global : -1;
    pthread_mutex_lock(&lock);
    const int changed = which != scene || (which == RUNTIME_SCENE_MODE && chosen != mode);
    const int new_stage = stage >= 0 && stage != map;
    scene = which;
    if (which == RUNTIME_SCENE_MODE) mode = chosen;
    if (stage >= 0 || which != RUNTIME_SCENE_MODE) map = stage;
    pthread_mutex_unlock(&lock);
    if (changed) printf("[state] %s\n", runtime_state_name(which, chosen, 0));
    if (new_stage) printf("[state] stage map%02d\n", stage);
}

void runtime_state_text(char *out, size_t size) {
    pthread_mutex_lock(&lock);
    const int which = scene, chosen = mode;
    pthread_mutex_unlock(&lock);
    snprintf(out, size, "%s", runtime_state_name(which, chosen, which == RUNTIME_SCENE_MODE && runtime_online_in_room()));
}
