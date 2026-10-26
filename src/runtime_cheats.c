/* The in-game menu's cheats (Game tab; minustwo.ini cheat_no_damage, cheat_infinite_breath).
 * Each kaiju's gauges are updated every frame (guest 0xab670) from tuning values in its player's
 * entry of the game's player table (0x79aa10 in the reference build, 0xd0 bytes an entry;
 * Godzilla, the one you play, is entry 0):
 *   +0x34  temperature gained a frame (x100); a full temperature (100) is one atomic breath
 *   +0x38  health ratio above which health regenerates, +0x3c health regained a frame,
 *   +0x48  regeneration on (the stages turn it on).
 * While a cheat is on, a thread keeps entry 0's values at ones that refill the breath and the
 * health every frame, and puts the game's own values back when it is turned off (a stage start
 * writes them again; the thread then saves those). The table is found through the code that
 * resets it, and the fields through the update that reads them: a build whose code differs has
 * no cheats, and the menu says so. */
#define _GNU_SOURCE
#include "runtime.h"
#include "signature.h"
#include "gpu/mtgpu.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The table reset (guest 0x16fa10): lea r12, [rip + table]. */
static const MtSignature table_ref = {
    "55 48 89 e5 41 57 41 56 41 55 41 54 53 50 4c 8d 25 ?? ?? ?? ?? 41 c7 84 24 c0 9c 27 00 00 00 00 00", 17};
/* The gauge update's reads of +0x34, +0x48, +0x38 and +0x3c through the kaiju's entry (+0x818). */
static const MtSignature field_reads[] = {
    {"48 8b 83 18 08 00 00 c5 fa 10 40 34 c5 fa 59 05 ?? ?? ?? ?? 4c 89 f7 e8 ?? ?? ?? ??", 0},
    {"48 8b 83 18 08 00 00 83 78 48 00 0f 84 ?? ?? ?? ?? f7 83 3c 06 00 00 00 10 10 00 0f 85 ?? ?? ?? ??", 0},
    {"c5 fa 10 50 38 c5 fa 10 8b 60 09 00 00 c5 fa 10 83 64 09 00 00 c5 f8 2e c1 c5 e0 57 db 77 08 "
     "c5 f2 5e 9b 68 09 00 00 c5 f8 2e da 76 31 c5 f2 58 48 3c", 0},
};

typedef struct {
    unsigned offset, cheat;
    uint32_t value;           /* what the cheat keeps there */
    uint32_t original; int saved; /* the game's value while the cheat holds the field */
} Field;
static Field fields[] = {
    {0x34, MTGPU_CHEAT_INFINITE_BREATH, 0x41200000, 0, 0}, /* 10.0: a breath's temperature in a frame */
    {0x38, MTGPU_CHEAT_NO_DAMAGE, 0xbf800000, 0, 0},       /* -1.0: regenerate at any health */
    {0x3c, MTGPU_CHEAT_NO_DAMAGE, 0x47c35000, 0, 0},       /* 100000.0: back to full in a frame */
    {0x48, MTGPU_CHEAT_NO_DAMAGE, 1, 0, 0},
};
static unsigned char *player; /* entry 0 */

/* Entry 0 of the player table in the image, or NULL when this build's code differs. */
static unsigned char *find_player(unsigned char *image, uint64_t size) {
    const uint64_t ref = mt_signature_find(image, size, &table_ref);
    if (ref == UINT64_MAX) return NULL;
    for (size_t i = 0; i < sizeof(field_reads) / sizeof(*field_reads); ++i)
        if (mt_signature_find(image, size, &field_reads[i]) == UINT64_MAX) return NULL;
    int32_t displacement;
    memcpy(&displacement, image + ref, 4);
    const uint64_t table = ref + 4 + (uint64_t)(int64_t)displacement;
    return table + 0xd0 <= size ? image + table : NULL;
}

/* One pass: the fields of the cheats in `on` hold their values, the others get the game's back. */
static void apply(unsigned on) {
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        Field *f = &fields[i];
        uint32_t *slot = (uint32_t *)(player + f->offset);
        const uint32_t now = __atomic_load_n(slot, __ATOMIC_RELAXED);
        if (on & f->cheat) {
            if (now != f->value) {
                f->original = now;
                f->saved = 1;
                __atomic_store_n(slot, f->value, __ATOMIC_RELAXED);
            }
        } else if (f->saved) {
            if (now == f->value) __atomic_store_n(slot, f->original, __ATOMIC_RELAXED);
            f->saved = 0;
        }
    }
}

static void *hold(void *unused) {
    (void)unused;
    for (;;) {
        const unsigned on = (unsigned)mtgpu_cheats();
        apply(on);
        const struct timespec pause = {0, (on ? 10 : 100) * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}

void runtime_cheats_configure(unsigned char *image, uint64_t size) {
    player = find_player(image, size);
    mtgpu_cheats_found(player != NULL);
    if (!player) {
        puts("Cheats: the player table was not found in this game build: no cheats");
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, hold, NULL)) {
        mtgpu_cheats_found(0);
        puts("Cheats: cannot start their thread: no cheats");
        return;
    }
    pthread_detach(thread);
    printf("Cheats: player table at image offset 0x%" PRIx64 "\n", (uint64_t)(player - image));
}
