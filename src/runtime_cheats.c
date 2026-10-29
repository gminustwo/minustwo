/* The in-game menu's cheats (Game tab; minustwo.ini cheat_no_damage, cheat_infinite_breath,
 * cheat_all_kaiju).
 * Each kaiju's gauges are updated every frame (guest 0xab670) from tuning values in its player's
 * entry of the game's player table (0x79aa10 in the reference build, 0xd0 bytes an entry;
 * Godzilla, the one you play, is entry 0):
 *   +0x34  temperature gained a frame (x100); a full temperature (100) is one atomic breath
 *   +0x38  health ratio above which health regenerates, +0x3c health regained a frame,
 *   +0x48  regeneration on (the stages turn it on).
 * While a cheat is on, a thread keeps entry 0's values at ones that refill the breath and the
 * health every frame, and puts the game's own values back when it is turned off (a stage start
 * writes them again; the thread then saves those).
 * The save data (0x6c40a0, the bytes of DATA.BIN) has a 0x524-byte record for each kaiju at
 * +0x88 + id * 0x524, ids 1 to 23 (11 is an unused slot), the id first. Bytes +4 to +7 unlock
 * the kaiju in the game's modes: a new save sets them for Godzilla, the game's own unlock
 * (0x161940) all four. While Unlock all kaiju is on, the thread sets them in every record that
 * holds its id (a save is loaded); the game writes them to DATA.BIN when it next saves, so the
 * kaiju stay unlocked after the cheat is turned off.
 * The table and the save data are found through code that references them, and the fields
 * through code that reads them: a build whose code differs lacks those cheats, and the menu
 * says so. The addresses here are the reference build's (01.01); 01.00 has the same table and
 * records (0x79a8f0, 0x6c3f80), and its code that differs has signatures of its own. */
#define _GNU_SOURCE
#include "runtime.h"
#include "signature.h"
#include "gpu/mtgpu.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The table reset (guest 0x16fa10): lea r12, [rip + table], then a store far into the block the
 * table starts (+0x279cc0; 01.00 has that block 8 bytes shorter there: +0x279cb8). */
static const MtSignature table_ref = {
    "55 48 89 e5 41 57 41 56 41 55 41 54 53 50 4c 8d 25 ?? ?? ?? ?? 41 c7 84 24 ?? ?? ?? ?? 00 00 00 00", 17};
/* The gauge update's reads of +0x34, +0x48, +0x38 and +0x3c through the kaiju's entry (+0x818). */
static const MtSignature field_reads[] = {
    {"48 8b 83 18 08 00 00 c5 fa 10 40 34 c5 fa 59 05 ?? ?? ?? ?? 4c 89 f7 e8 ?? ?? ?? ??", 0},
    {"48 8b 83 18 08 00 00 83 78 48 00 0f 84 ?? ?? ?? ?? f7 83 3c 06 00 00 00 10 10 00 0f 85 ?? ?? ?? ??", 0},
    {"c5 fa 10 50 38 c5 fa 10 8b 60 09 00 00 c5 fa 10 83 64 09 00 00 c5 f8 2e c1 c5 e0 57 db 77 08 "
     "c5 f2 5e 9b 68 09 00 00 c5 f8 2e da 76 31 c5 f2 58 48 3c", 0},
};
/* The save summary (guest 0x1626c0): lea rbx, [rip + save], then its sum over the kaiju
 * records from lea rdi, [rbx + 0x88]. */
static const MtSignature save_ref = {
    "49 89 fe 48 8d 1d ?? ?? ?? ?? 8b 83 80 46 0d 00 41 89 46 0c 8a 83 b0 46 0d 00 41 88 46 0a "
    "8a 83 f4 46 0d 00 41 88 46 0b 8b 83 c0 46 0d 00 41 89 46 10 48 8d bb 88 00 00 00 e8", 6};
/* That sum's walk from record 1 in steps of 0x524 (0x1624e0), and the read of unlock flag n at
 * +4 + n (0x161930) before the unlock of all four, which looks at the id at +0 (0x161940). */
static const MtSignature kaiju_reads[] = {
    {"49 89 ff 4c 8d 25 ?? ?? ?? ?? 49 83 c4 10 49 81 c7 24 05 00 00 31 db 45 31 f6", 0},
    {"48 63 c6 8a 44 07 04 c3 0f 1f 84 00 00 00 00 00 55 48 89 e5 41 56 53 48 89 fb 83 3b 08", 0},
};
/* The same in 01.00, compiled otherwise: the walk (0x161e90 there) moves a pointer from record 1,
 * and the unlock of all four (0x161430) writes them at once, without 01.01's check of kaiju 8. */
static const MtSignature kaiju_reads_100[] = {
    {"48 89 fb 48 81 c3 24 05 00 00 45 31 f6 41 bf 10 00 00 00 4c 8d 25 ?? ?? ?? ??", 0},
    {"48 63 c6 8a 44 07 04 c3 0f 1f 84 00 00 00 00 00 c7 47 04 01 01 01 01 c3", 0},
};
enum { KAIJU_RECORDS = 0x88, KAIJU_RECORD_SIZE = 0x524, KAIJU_LAST = 23, KAIJU_UNUSED = 11 };

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
static unsigned char *save;

/* The `bytes` of data that ref's rip-relative operand points to, when they are inside the image
 * and the code that reads them (`reads`) is there too; else NULL. */
static unsigned char *find_data(unsigned char *image, uint64_t size, const MtSignature *ref,
                                const MtSignature *reads, size_t count, uint64_t bytes) {
    const uint64_t at = mt_signature_find(image, size, ref);
    if (at == UINT64_MAX) return NULL;
    for (size_t i = 0; i < count; ++i)
        if (mt_signature_find(image, size, &reads[i]) == UINT64_MAX) return NULL;
    int32_t displacement;
    memcpy(&displacement, image + at, 4);
    const uint64_t data = at + 4 + (uint64_t)(int64_t)displacement;
    return data + bytes <= size ? image + data : NULL;
}

/* One pass: the fields of the cheats in `on` hold their values, the others get the game's back;
 * with Unlock all kaiju, every kaiju's unlock flags are set. */
static void apply(unsigned on) {
    for (size_t i = 0; player && i < sizeof(fields) / sizeof(*fields); ++i) {
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
    if (!save || !(on & MTGPU_CHEAT_ALL_KAIJU)) return;
    for (uint32_t id = 1; id <= KAIJU_LAST; ++id) {
        unsigned char *record = save + KAIJU_RECORDS + id * KAIJU_RECORD_SIZE;
        if (id == KAIJU_UNUSED || __atomic_load_n((uint32_t *)record, __ATOMIC_RELAXED) != id) continue;
        for (unsigned flag = 4; flag < 8; ++flag)
            if (!__atomic_load_n(record + flag, __ATOMIC_RELAXED)) __atomic_store_n(record + flag, 1, __ATOMIC_RELAXED);
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
    player = find_data(image, size, &table_ref, field_reads, sizeof(field_reads) / sizeof(*field_reads), 0xd0);
    save = find_data(image, size, &save_ref, kaiju_reads, sizeof(kaiju_reads) / sizeof(*kaiju_reads),
                     KAIJU_RECORDS + (KAIJU_LAST + 1) * KAIJU_RECORD_SIZE);
    if (!save)
        save = find_data(image, size, &save_ref, kaiju_reads_100, sizeof(kaiju_reads_100) / sizeof(*kaiju_reads_100),
                         KAIJU_RECORDS + (KAIJU_LAST + 1) * KAIJU_RECORD_SIZE);
    const int found = (player ? MTGPU_CHEAT_NO_DAMAGE | MTGPU_CHEAT_INFINITE_BREATH : 0) |
                      (save ? MTGPU_CHEAT_ALL_KAIJU : 0);
    if (!player) puts("Cheats: the player table was not found in this game build: no health or breath cheats");
    if (!save) puts("Cheats: the kaiju records were not found in this game build: no kaiju unlock");
    if (!found) {
        mtgpu_cheats_found(0);
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, hold, NULL)) {
        mtgpu_cheats_found(0);
        puts("Cheats: cannot start their thread: no cheats");
        return;
    }
    pthread_detach(thread);
    mtgpu_cheats_found(found);
    if (player) printf("Cheats: player table at image offset 0x%" PRIx64 "\n", (uint64_t)(player - image));
    if (save) printf("Cheats: save data at image offset 0x%" PRIx64 "\n", (uint64_t)(save - image));
}
