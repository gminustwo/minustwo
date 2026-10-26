/* The menu's cheats (src/runtime_cheats.c): the player table found by signature, the cheats'
 * values held in its entry 0 and the game's own put back. */
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

static uint32_t bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static uint32_t get(unsigned offset) { uint32_t v; memcpy(&v, player + offset, 4); return v; }
static void put(unsigned offset, uint32_t v) { memcpy(player + offset, &v, 4); }

int main(void) {
    enum { SIZE = 0x10000, TABLE = 0x8000 };
    unsigned char *image = calloc(SIZE, 1);
    assert(image);
    runtime_cheats_configure(image, SIZE); /* code unlike the game's: no cheats, no thread */
    assert(found_reported == 0 && !player);

    const uint64_t ref = place(image, 0x100, &table_ref);
    const int32_t displacement = (int32_t)(TABLE - (ref + 4));
    memcpy(image + ref, &displacement, 4);
    assert(!find_player(image, SIZE)); /* the reads of the fields are missing */
    for (size_t i = 0; i < sizeof(field_reads) / sizeof(*field_reads); ++i)
        place(image, 0x1000 + i * 0x100, &field_reads[i]);
    player = find_player(image, SIZE);
    assert(player == image + TABLE);
    assert(!find_player(image, TABLE + 0x80)); /* the entry must be inside the image */

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

    free(image);
    puts("PASS: cheats");
    return 0;
}
