/* Ultrawide screens (21:9, 32:9). The game draws for 16:9. With a render resolution wider than that,
 * run.py sets MT_ASPECT=<width>x<height> (MT_SCENE_SIZE, the renderer's test override, counts too):
 * the renderer draws the game's 1920x1080 targets at that full width (vk_rasterizer.cpp) and this
 * file makes the game's cameras as wide, so the view widens instead of stretching and what the
 * cameras draw of the interface keeps its shape in the middle of the screen.
 * - The stage, HUD and menu cameras the code sets up store their aspect, 16:9 (0x3fe38e39), as an
 *   immediate: mov dword [camera + 0xd0], imm32, or mov eax, imm32 before the store (nine sites in
 *   01.01). Each becomes the screen's aspect.
 * - The cameras of the menus' layouts (their own class: field of view +0x298, aspect +0x29c, near
 *   +0x2a0, far +0x2a4) take the aspect from the display size (1920 / 1080) or from the layout's
 *   data, and rebuild their projection in one function (0x149c90 in 01.01; 01.00 has the same
 *   code), whose load of the aspect (vbroadcastss xmm11, [rbx + 0x29c]) becomes a jump to a stub
 *   that loads the screen's aspect instead whenever the camera's is 16:9.
 * Menu text is drawn without a camera (an orthographic matrix of the 1920x1080 pixels per string):
 * the renderer keeps those draws 16:9 in the middle (vk_rasterizer.cpp, screen_space_draw). */
#include "runtime.h"
#include "signature.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char SIXTEEN_NINTHS[4] = {0x39, 0x8e, 0xe3, 0x3f}; /* 16/9 as a float */
/* The layout cameras' projection update: push rbp; mov rbp, rsp; push rbx; push rax; mov rbx, rdi; its
 * loads of near (+0x2a0), far (+0x2a4) and the field of view (+0x298), then of the aspect (the site,
 * 9 bytes). Its next use of rax is a write and its calls come before any read of the flags. */
static const MtSignature layout_projection = {
    "55 48 89 e5 53 50 48 89 fb c4 62 79 18 8b a0 02 00 00 c4 62 79 18 93 a4 02 00 00 "
    "c4 e2 79 18 93 98 02 00 00 c4 62 79 18 9b 9c 02 00 00", 36};
enum { SITE_BYTES = 9 };

/* The screen's aspect from "<width>x<height>" (or "<width>:<height>"), 0 when the text is not one. */
float runtime_aspect_parse(const char *text) {
    unsigned width = 0, height = 0;
    char separator = 0;
    if (!text || sscanf(text, "%u%c%u", &width, &separator, &height) != 3 || (separator != 'x' && separator != ':') ||
        !width || !height)
        return 0;
    return (float)width / (float)height;
}

/* Whether the 16:9 at `p` is the immediate of mov r32, imm32 or mov dword [base + disp], imm32. */
static int camera_immediate(const unsigned char *p) {
    if (p[-1] >= 0xb8 && p[-1] <= 0xbf) return 1;
    const unsigned disp8 = p[-2], disp32 = p[-5]; /* the ModRM bytes: /0, no SIB */
    if (p[-3] == 0xc7 && disp8 >> 6 == 1 && (disp8 >> 3 & 7) == 0 && (disp8 & 7) != 4) return 1;
    return p[-6] == 0xc7 && disp32 >> 6 == 2 && (disp32 >> 3 & 7) == 0 && (disp32 & 7) != 4;
}

unsigned runtime_aspect_set_cameras(unsigned char *code, uint64_t size, float screen) {
    unsigned count = 0;
    for (uint64_t at = 8; at + 4 <= size; ++at) {
        if (code[at] != SIXTEEN_NINTHS[0] || memcmp(code + at, SIXTEEN_NINTHS, 4) || !camera_immediate(code + at)) continue;
        memcpy(code + at, &screen, 4);
        ++count;
    }
    return count;
}

/* The stub for the aspect load at `site`: eax = the camera's aspect, the screen's when it is 16:9
 * (within a few units in the last place: layout files write 1.77778); xmm11 = eax in each lane;
 * back to the instruction after the site. Returns its size. */
static size_t emit_layout_stub(unsigned char *code, const unsigned char *site, float screen) {
    static const unsigned char load[] = {0x8b, 0x83, 0x9c, 0x02, 0x00, 0x00}; /* mov eax, [rbx + 0x29c] */
    size_t n = 0;
    memcpy(code + n, load, sizeof(load)); n += sizeof(load);
    code[n++] = 0x2d; code[n++] = 0x20; code[n++] = 0x8e; code[n++] = 0xe3; code[n++] = 0x3f; /* sub eax, 0x3fe38e20 */
    code[n++] = 0x83; code[n++] = 0xf8; code[n++] = 0x40;                  /* cmp eax, 0x40 */
    code[n++] = 0x73; code[n++] = 0x07;                                    /* jae keep */
    code[n++] = 0xb8; memcpy(code + n, &screen, 4); n += 4;                /* mov eax, screen */
    code[n++] = 0xeb; code[n++] = sizeof(load);                            /* jmp set */
    memcpy(code + n, load, sizeof(load)); n += sizeof(load);               /* keep: mov eax, [rbx + 0x29c] */
    code[n++] = 0xc5; code[n++] = 0x79; code[n++] = 0x6e; code[n++] = 0xd8; /* set: vmovd xmm11, eax */
    code[n++] = 0xc4; code[n++] = 0x41; code[n++] = 0x79; code[n++] = 0x70; code[n++] = 0xdb; code[n++] = 0x00; /* vpshufd xmm11, xmm11, 0 */
    const int32_t back = (int32_t)((site + SITE_BYTES) - (code + n + 5));
    code[n++] = 0xe9; memcpy(code + n, &back, 4); n += 4;                  /* jmp site + 9 */
    return n;
}

int runtime_aspect_hook_layouts(unsigned char *code, uint64_t size, unsigned char *stub, size_t bytes, float screen) {
    const uint64_t at = mt_signature_find(code, size, &layout_projection);
    if (at == UINT64_MAX || bytes < 64) return 0;
    unsigned char *site = code + at;
    const int64_t distance = (int64_t)(stub - (site + 5)), back = (int64_t)((site + SITE_BYTES) - (stub + 64));
    if (distance != (int32_t)distance || back != (int32_t)back) return 0;
    emit_layout_stub(stub, site, screen);
    const int32_t rel = (int32_t)distance;
    site[0] = 0xe9;
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0xcc, SITE_BYTES - 5);
    return 1;
}

unsigned runtime_aspect_install(unsigned char *code, uint64_t size, unsigned char *stubs, size_t bytes) {
    const char *scene = getenv("MT_SCENE_SIZE");
    float screen = runtime_aspect_parse(getenv("MT_ASPECT"));
    if (scene && scene[0]) screen = runtime_aspect_parse(scene);
    if (screen <= 16.0f / 9.0f + 0.01f) return 0;
    const unsigned cameras = runtime_aspect_set_cameras(code, size, screen);
    const int layouts = runtime_aspect_hook_layouts(code, size, stubs, bytes, screen);
    printf("Aspect: %.3f (wider than 16:9): %u camera aspects set, layout cameras %s\n", screen, cameras,
           layouts ? "widened" : "not found in this game build");
    return cameras + (unsigned)layouts;
}
