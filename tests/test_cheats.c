/* The menu's cheats (src/runtime_cheats.c): the player table found by signature, the cheats'
 * values held in its entry 0 and the game's own put back; the save data's kaiju records found
 * the same way and every kaiju unlocked. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_cheats.c"
#include <assert.h>
#include <stdlib.h>

static int found_reported = -1;
int mtgpu_cheats(void) { return 0; }
void mtgpu_cheats_found(int found) { found_reported = found; }

/* A signature's bytes at `at` ("??" as 0); the site. */
static uint64_t place(unsigned char *image, uint64_t at, const MtSignature *sig) {
    int pattern[MT_SIGNATURE_MAX];
    const unsigned n = mt_signature_parse(sig->pattern, pattern);
    assert(n);
    for (unsigned i = 0; i < n; ++i) image[at + i] = pattern[i] < 0 ? 0 : (unsigned char)pattern[i];
    return at + sig->at;
}

/* A rip-relative reference at `ref` (its rel32) to `target`. */
static void point(unsigned char *image, uint64_t ref, uint64_t target) {
    const int32_t displacement = (int32_t)(target - (ref + 4));
    memcpy(image + ref, &displacement, 4);
}

static uint32_t bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static uint32_t get(unsigned offset) { uint32_t v; memcpy(&v, player + offset, 4); return v; }
static void put(unsigned offset, uint32_t v) { memcpy(player + offset, &v, 4); }

int main(void) {
    enum { SIZE = 0x10000, TABLE = 0x8000 };
    unsigned char *image = calloc(SIZE, 1);
    assert(image);
    runtime_cheats_configure(image, SIZE); /* code unlike the game's: no cheats, no thread */
    assert(found_reported == 0 && !player);

    /* The table reset as 01.01 and 01.00 have it (the store goes to another offset): both match. */
    static const MtSignature resets[] = {
        {"55 48 89 e5 41 57 41 56 41 55 41 54 53 50 4c 8d 25 eb af 62 00 41 c7 84 24 c0 9c 27 00 00 00 00 00", 17},
        {"55 48 89 e5 41 57 41 56 41 55 41 54 53 50 4c 8d 25 db b5 62 00 41 c7 84 24 b8 9c 27 00 00 00 00 00", 17},
    };
    for (size_t i = 0; i < sizeof(resets) / sizeof(*resets); ++i) {
        const uint64_t site = place(image, 0x100, &resets[i]);
        assert(mt_signature_find(image, SIZE, &table_ref) == site);
        memset(image + 0x100, 0, 0x100);
    }
    point(image, place(image, 0x100, &table_ref), TABLE);
    runtime_cheats_configure(image, SIZE); /* the reads of the fields are missing */
    assert(found_reported == 0 && !player && !save);
    for (size_t i = 0; i < sizeof(field_reads) / sizeof(*field_reads); ++i)
        place(image, 0x1000 + i * 0x100, &field_reads[i]);
    player = find_data(image, SIZE, &table_ref, field_reads, sizeof(field_reads) / sizeof(*field_reads), 0xd0);
    assert(player == image + TABLE);
    /* the entry must be inside the image */
    assert(!find_data(image, TABLE + 0x80, &table_ref, field_reads, sizeof(field_reads) / sizeof(*field_reads), 0xd0));

    /* The game's values for a stage. */
    put(0x34, bits(0.001111f)); put(0x38, bits(0.3f)); put(0x3c, bits(0.016667f)); put(0x48, 0);
    apply(0);
    assert(get(0x34) == bits(0.001111f) && get(0x48) == 0);
    apply(MTGPU_CHEAT_INFINITE_BREATH);
    assert(get(0x34) == bits(10.0f) && get(0x38) == bits(0.3f) && get(0x3c) == bits(0.016667f) && get(0x48) == 0);
    apply(MTGPU_CHEAT_INFINITE_BREATH | MTGPU_CHEAT_NO_DAMAGE);
    assert(get(0x38) == bits(-1.0f) && get(0x3c) == bits(100000.0f) && get(0x48) == 1);

    /* A stage start writes the game's values again: those are the ones put back. */
    put(0x34, bits(0.002f)); put(0x48, 0);
    apply(MTGPU_CHEAT_INFINITE_BREATH | MTGPU_CHEAT_NO_DAMAGE);
    assert(get(0x34) == bits(10.0f) && get(0x48) == 1);
    apply(MTGPU_CHEAT_NO_DAMAGE);
    assert(get(0x34) == bits(0.002f) && get(0x48) == 1);
    apply(0);
    assert(get(0x38) == bits(0.3f) && get(0x3c) == bits(0.016667f) && get(0x48) == 0);

    /* Turned off after the game wrote a value of its own: that one stays. */
    apply(MTGPU_CHEAT_INFINITE_BREATH);
    put(0x34, bits(0.003f));
    apply(0);
    assert(get(0x34) == bits(0.003f));
    apply(0);
    assert(get(0x34) == bits(0.003f));

    /* The save data: kaiju records found once the code that walks them and reads their flags is there. */
    enum { SAVE = 0x10000, BIG = 0x20000, RECORDS = KAIJU_RECORDS + (KAIJU_LAST + 1) * KAIJU_RECORD_SIZE };
    unsigned char *big = calloc(BIG, 1);
    assert(big);
    memcpy(big, image, SIZE);
    point(big, place(big, 0x2000, &save_ref), SAVE);
    assert(!find_data(big, BIG, &save_ref, kaiju_reads, 2, RECORDS));
    place(big, 0x2100, &kaiju_reads[0]);
    place(big, 0x2200, &kaiju_reads[1]);
    save = find_data(big, BIG, &save_ref, kaiju_reads, 2, RECORDS);
    assert(save == big + SAVE);
    assert(!find_data(big, SAVE + RECORDS - 1, &save_ref, kaiju_reads, 2, RECORDS));

    /* No save loaded yet (the records hold no ids): nothing is written. */
    apply(MTGPU_CHEAT_ALL_KAIJU);
    for (unsigned i = 0; i < RECORDS; ++i) assert(!save[i]);
    /* A save: every record has its id; Godzilla (1) unlocked, Godzilla 1964 (8) in one mode. */
    unsigned char *record[KAIJU_LAST + 2];
    for (uint32_t id = 0; id <= KAIJU_LAST + 1; ++id) {
        record[id] = save + KAIJU_RECORDS + id * KAIJU_RECORD_SIZE;
        if (id <= KAIJU_LAST) memcpy(record[id], &id, 4);
    }
    memset(record[1] + 4, 1, 4);
    record[8][4] = 1;
    record[5][8] = 0x7f; /* the record goes on after the flags */
    unsigned char before[RECORDS];
    memcpy(before, save, RECORDS);
    apply(MTGPU_CHEAT_NO_DAMAGE);
    assert(!memcmp(before, save, RECORDS)); /* not with the other cheats alone */
    apply(MTGPU_CHEAT_ALL_KAIJU);
    for (uint32_t id = 0; id <= KAIJU_LAST; ++id) {
        const int unlocked = id && id != KAIJU_UNUSED;
        for (unsigned flag = 4; flag < 8; ++flag) assert(record[id][flag] == unlocked);
    }
    for (unsigned i = 0; i < RECORDS; ++i) { /* nothing but the flags changed */
        const unsigned in_record = (i - KAIJU_RECORDS) % KAIJU_RECORD_SIZE;
        if (i < KAIJU_RECORDS || in_record < 4 || in_record >= 8) assert(save[i] == before[i]);
    }
    assert(record[5][8] == 0x7f && record[KAIJU_LAST + 1][4] == 0); /* past the last kaiju */
    /* A record that does not hold its id (being written) is left alone. */
    memset(record[3] + 4, 0, 4);
    record[3][0] = 0;
    apply(MTGPU_CHEAT_ALL_KAIJU);
    assert(!record[3][4] && record[2][4]);
    /* Turned off: the unlocks stay (the game has them in its save now). */
    record[3][0] = 3;
    apply(0);
    assert(record[2][4] && record[2][7] && !record[3][4]);

    /* 01.00 walks the records and unlocks them with other code (kaiju_reads_100). */
    memset(big + 0x2100, 0, 0x200);
    assert(!find_data(big, BIG, &save_ref, kaiju_reads, 2, RECORDS));
    place(big, 0x2100, &kaiju_reads_100[0]);
    assert(!find_data(big, BIG, &save_ref, kaiju_reads_100, 2, RECORDS));
    place(big, 0x2200, &kaiju_reads_100[1]);
    assert(!find_data(big, BIG, &save_ref, kaiju_reads, 2, RECORDS));
    assert(find_data(big, BIG, &save_ref, kaiju_reads_100, 2, RECORDS) == big + SAVE);

    /* Both found (the records through 01.00's code): all three cheats. The thread started here
     * reads big until the test ends. */
    save = player = NULL;
    runtime_cheats_configure(big, BIG);
    assert(found_reported == (MTGPU_CHEAT_NO_DAMAGE | MTGPU_CHEAT_INFINITE_BREATH | MTGPU_CHEAT_ALL_KAIJU));
    assert(player == big + TABLE && save == big + SAVE);
    free(image);
    puts("PASS: cheats");
    return 0;
}
