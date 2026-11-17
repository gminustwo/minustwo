/* Ultrawide screens (src/runtime_aspect.c): the screen's aspect from MT_ASPECT's text; the 16:9
 * immediates of the camera set-ups become it (the forms the game uses, and no other bytes); and the
 * menu layouts' projection update, patched, runs and loads the screen's aspect for a 16:9 camera and
 * the camera's own for any other. */
#define _GNU_SOURCE /* MAP_ANONYMOUS under -std=c11 */
#undef NDEBUG       /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_aspect.c"
#include <assert.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

enum { PAGE = 4096, CODE = 2 * PAGE, FUNCTION_AT = 0x200 };

/* The update's bytes (the signature), then: vmovss [rbx + 0x2b0], xmm11 (what the stub loaded, for
 * the test to read); add rsp, 8; pop rbx; pop rbp; ret. */
static const unsigned char tail[] = {0xc5, 0x7a, 0x11, 0x9b, 0xb0, 0x02, 0x00, 0x00,
                                     0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3};
typedef void (ABI *Update)(void *camera);

static float run(Update update, float camera_aspect) {
    _Alignas(16) unsigned char camera[0x300] = {0};
    const float fov = 0.5236f, near_ = 0.1f, far_ = 10000.0f;
    memcpy(camera + 0x298, &fov, 4);
    memcpy(camera + 0x29c, &camera_aspect, 4);
    memcpy(camera + 0x2a0, &near_, 4);
    memcpy(camera + 0x2a4, &far_, 4);
    update(camera);
    float loaded;
    memcpy(&loaded, camera + 0x2b0, 4);
    return loaded;
}

int main(void) {
    assert(fabsf(runtime_aspect_parse("3840x1080") - 32.0f / 9.0f) < 1e-6f);
    assert(fabsf(runtime_aspect_parse("5120:1440") - 32.0f / 9.0f) < 1e-6f);
    assert(fabsf(runtime_aspect_parse("3440x1440") - 3440.0f / 1440.0f) < 1e-6f);
    assert(runtime_aspect_parse(NULL) == 0 && runtime_aspect_parse("") == 0 && runtime_aspect_parse("wide") == 0);
    assert(runtime_aspect_parse("0x1080") == 0 && runtime_aspect_parse("1920-1080") == 0);

    /* The camera set-ups' forms: mov dword [rbx + 0x3d0], imm32; mov dword [rbp - 0x30], imm32;
     * mov eax, imm32. Not: the same float in data, or behind a SIB store. */
    static const unsigned char sites[] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0xc7, 0x83, 0xd0, 0x03, 0x00, 0x00, 0x39, 0x8e, 0xe3, 0x3f,       /* +0x0e */
        0xc7, 0x45, 0xd0, 0x39, 0x8e, 0xe3, 0x3f,                         /* +0x15 */
        0xb8, 0x39, 0x8e, 0xe3, 0x3f,                                     /* +0x1a */
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x39, 0x8e, 0xe3, 0x3f,       /* data: +0x25 */
        0xc7, 0x84, 0x24, 0x10, 0x00, 0x00, 0x00, 0x39, 0x8e, 0xe3, 0x3f, /* [rsp + 0x10]: +0x2f */
    };
    unsigned char code[sizeof(sites)];
    memcpy(code, sites, sizeof(code));
    const float screen = 32.0f / 9.0f;
    assert(runtime_aspect_set_cameras(code, sizeof(code), screen) == 3);
    for (unsigned at = 0; at < sizeof(code); ++at) {
        const int changed = at == 0x0e || at == 0x15 || at == 0x1a;
        if (changed) {
            float f;
            memcpy(&f, code + at, 4);
            assert(f == screen);
            at += 3;
        } else {
            assert(code[at] == sites[at]);
        }
    }

    /* The layout cameras' projection update, as in the game, then hooked. */
#ifdef _WIN32
    unsigned char *region = VirtualAlloc(NULL, CODE + PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(region);
#else
    unsigned char *region = mmap(NULL, CODE + PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region != MAP_FAILED);
#endif
    unsigned char *stub = region + CODE;
    memset(region, 0xcc, CODE);
    assert(!runtime_aspect_hook_layouts(region, CODE, stub, PAGE, screen)); /* not there: nothing changed */
    int pattern[MT_SIGNATURE_MAX];
    const unsigned n = mt_signature_parse(layout_projection.pattern, pattern);
    assert(n == 45 && layout_projection.at == 36);
    unsigned char *function = region + FUNCTION_AT;
    for (unsigned k = 0; k < n; ++k) function[k] = (unsigned char)pattern[k];
    memcpy(function + n, tail, sizeof(tail));
    const Update update = (Update)(void *)function;
    assert(run(update, 16.0f / 9.0f) == 16.0f / 9.0f); /* the game's own load */

    assert(runtime_aspect_hook_layouts(region, CODE, stub, PAGE, screen));
    unsigned char *site = function + layout_projection.at;
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    assert(site[0] == 0xe9 && site + 5 + rel == stub);
    for (unsigned k = 5; k < SITE_BYTES; ++k) assert(site[k] == 0xcc);
    assert(run(update, 16.0f / 9.0f) == screen);  /* the display's 1920 / 1080 */
    assert(run(update, 1.77778f) == screen);       /* a layout file's 16:9 */
    assert(run(update, 1.0f) == 1.0f);             /* other cameras keep theirs */
    assert(run(update, 21.0f / 9.0f) == 21.0f / 9.0f);
    assert(run(update, 1.7f) == 1.7f);
    puts("aspect: MT_ASPECT parsed; 3 camera immediates set; layout cameras load the screen's aspect for 16:9 only");
    return 0;
}
