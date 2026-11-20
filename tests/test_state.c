/* runtime_state.c: the screen and mode the files the game opens and the main menu's stored item tell, and the
 * signature of that store in the bytes of the three builds' code. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_state.c"
#include <assert.h>

static int online;
int runtime_online_in_room(void) { return online; }

int main(void) {
    int stage;
    assert(runtime_state_scene_of("/app0/mn_title_en.pac", &stage) == RUNTIME_SCENE_TITLE && stage == -1);
    assert(runtime_state_scene_of("/app0/mn_main_fr.pac", &stage) == RUNTIME_SCENE_MAIN_MENU);
    assert(runtime_state_scene_of("mn_option.pac", &stage) == RUNTIME_SCENE_SETTINGS);
    const char *const modes[] = {"/app0/mn_camp_en.pac", "/app0/mn_vs_de.pac", "/app0/mn_evol_it.pac",
                                 "/app0/diorama_EN.pac", "/app0/mn_extra_es.pac"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); ++i)
        assert(runtime_state_scene_of(modes[i], &stage) == RUNTIME_SCENE_MODE && stage == -1);
    assert(runtime_state_scene_of("/app0/map51.pac", &stage) == RUNTIME_SCENE_MODE && stage == 51);
    assert(runtime_state_scene_of("/app0/map32.pac", &stage) == RUNTIME_SCENE_MODE && stage == 32);
    /* files that say nothing about the screen */
    const char *const quiet[] = {"/app0/dmap32a.pac", "/app0/map1.pac", "/app0/map32.mp4", "/app0/ch01a_00.pac",
                                 "/app0/game_en.pac", "/app0/system.pac", "/app0/mn_logo.pac", "/savedata0/DATA.BIN"};
    for (size_t i = 0; i < sizeof(quiet) / sizeof(*quiet); ++i) assert(runtime_state_scene_of(quiet[i], &stage) == -1);

    assert(!strcmp(runtime_state_name(RUNTIME_SCENE_STARTING, -1, 0), "Starting the game"));
    assert(!strcmp(runtime_state_name(RUNTIME_SCENE_MAIN_MENU, 3, 0), "Main menu"));
    assert(!strcmp(runtime_state_name(RUNTIME_SCENE_MODE, 0, 0), "God of Destruction Mode"));
    assert(!strcmp(runtime_state_name(RUNTIME_SCENE_MODE, 1, 0), "King of Kaiju Mode"));
    assert(!strcmp(runtime_state_name(RUNTIME_SCENE_MODE, 2, 1), "VS Mode (online)"));
    assert(!strcmp(runtime_state_name(RUNTIME_SCENE_MODE, 4, 0), "Diorama Mode"));
    assert(!strcmp(runtime_state_name(RUNTIME_SCENE_MODE, 9, 0), "Playing"));
    assert(!strcmp(runtime_state_name(RUNTIME_SCENE_MODE, -1, 0), "Playing"));

    /* The main menu's store in the three builds' bytes (01.01 at 0x2c783, 01.00 and the European 01.00 with
     * rdx for the save data), the global 0x650040 in each: an image of the code and the data. */
    static const unsigned char code_101[] = {
        0x48, 0x8d, 0x0d, 0xb6, 0x38, 0x62, 0x00, 0x89, 0x01, 0x48, 0x8d, 0x05, 0xbd, 0x65, 0x67, 0x00,
        0xc7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0xb4, 0x65, 0x67, 0x00, 0xc7, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0xf3, 0x78, 0x69, 0x00, 0x8b, 0x97, 0xd0, 0x01, 0x00, 0x00};
    static const unsigned char code_100[] = {
        0x48, 0x8d, 0x0d, 0x26, 0x39, 0x62, 0x00, 0x89, 0x01, 0x48, 0x8d, 0x05, 0x2d, 0x65, 0x67, 0x00,
        0xc7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0x04, 0x65, 0x67, 0x00, 0xc7, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x8d, 0x15, 0x43, 0x78, 0x69, 0x00, 0x8b, 0x8f, 0xd0, 0x01, 0x00, 0x00};
    static unsigned char image[0x650044];
    const struct { const unsigned char *code; size_t bytes; uint64_t at; } builds[] = {
        {code_101, sizeof(code_101), 0x2c783}, {code_100, sizeof(code_100), 0x2c713}};
    for (size_t b = 0; b < 2; ++b) {
        memset(image, 0, sizeof(image));
        memcpy(image + builds[b].at, builds[b].code, builds[b].bytes);
        int32_t chosen = 1;
        memcpy(image + 0x650040, &chosen, 4);
        runtime_state_configure(image, sizeof(image));
        assert(mode_global == (const volatile int32_t *)(image + 0x650040));
        /* the title, then King of Kaiju's menu, then back to the main menu */
        char text[64];
        runtime_state_text(text, sizeof(text));
        runtime_state_file("/app0/mn_title_en.pac");
        runtime_state_text(text, sizeof(text));
        assert(!strcmp(text, "Title screen"));
        runtime_state_file("/app0/mn_camp_en.pac");
        runtime_state_text(text, sizeof(text));
        assert(!strcmp(text, "King of Kaiju Mode"));
        runtime_state_file("/app0/map32.pac");
        runtime_state_file("/app0/ch02a_00.pac");
        runtime_state_text(text, sizeof(text));
        assert(!strcmp(text, "King of Kaiju Mode") && map == 32);
        runtime_state_file("/app0/mn_main_en.pac");
        runtime_state_text(text, sizeof(text));
        assert(!strcmp(text, "Main menu") && map == -1);
        chosen = 2;
        memcpy(image + 0x650040, &chosen, 4);
        runtime_state_file("/app0/mn_vs_en.pac");
        online = 1;
        runtime_state_text(text, sizeof(text));
        assert(!strcmp(text, "VS Mode (online)"));
        online = 0;
    }
    /* a build without the store: the modes are not told apart */
    memset(image, 0, sizeof(image));
    runtime_state_configure(image, sizeof(image));
    assert(!mode_global);
    runtime_state_file("/app0/mn_camp_en.pac");
    char text[64];
    runtime_state_text(text, sizeof(text));
    assert(!strcmp(text, "Playing"));
    puts("State: screens from files, the main menu's mode in 01.01 and 01.00 code, online VS PASS");
}
