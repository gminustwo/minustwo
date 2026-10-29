/* The render thread's waits for the GPU. Before it reuses one of its command buffers, the game's
 * render thread waits until the GPU's end-of-pipe label (in direct memory, through the pointer at
 * guest 0xa5a010) has passed the fence value of that buffer's last submission:
 *         mov rdi, [rcx]
 *         jmp test
 *   again: inc dword [rbp - 0xac]      ; a count of the turns, never read
 *         mov rdi, [rcx]
 *   test: cmp edi, esi
 *         js again
 * without a pause or a yield, so the thread kept a CPU core busy for as long as it waited: most of
 * every frame (96% of a core at 60 FPS), which a 4-core machine (Steam Deck) and its power budget
 * miss. Each of the four copies of that loop (guest 0x24673c, 0x24678e, 0x24b37a, 0x24b4fb in the
 * reference build 01.01; 01.00 has the same code) becomes a call to a stub that sleeps in the GPU
 * library until the command processor writes the label (mtgpu_wait_label), then loads the
 * register the loop leaves the label in. MT_LABEL_WAIT=0 keeps the game's loops. */
#include "runtime.h"
#include "signature.h"
#include "gpu/mtgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each loop from its jmp to the test (at) to its js; the fixed bytes before it are the loads of
 * the label and of the buffer's fence value (+0xe7d0 of its 0xe7f8-byte slot). */
static const MtSignature loops[] = {
    {"48 8b 10 43 8b 8c 3c d0 e7 00 00 eb 0b 66 90 ff 85 ?? ?? ?? ?? 48 8b 10 39 ca 78 f3", 11},
    {"48 8b 39 48 69 d2 f8 e7 00 00 41 8b b4 14 d0 e7 00 00 eb 09 ff 85 ?? ?? ?? ?? 48 8b 39 39 f7 78 f3", 18},
    {"48 8b 39 48 69 d2 f8 e7 00 00 8b b4 10 d0 e7 00 00 eb 0d 0f 1f 40 00 ff 85 ?? ?? ?? ?? 48 8b 39 39 f7 78 f3", 17},
    {"48 8b 10 43 8b 8c 25 d0 e7 00 00 4c 89 e7 4c 89 ee eb 0c 0f 1f 00 ff 85 ?? ?? ?? ?? 48 8b 10 39 ca 78 f3", 17},
};
enum { LOOPS = sizeof(loops) / sizeof(*loops), STUB_BYTES = 64 };

typedef struct {
    unsigned char *site, *end; /* the jmp, and the instruction after the js */
    int label, target, value;  /* registers (x86 numbers): label pointer, fence value, the loaded label */
} Loop;

/* The loop at `site` (its jmp): registers from its load and compare. 0 when it is not that loop. */
static int decode(unsigned char *site, Loop *loop) {
    if (site[0] != 0xeb) return 0;
    unsigned char *test = site + 2 + (signed char)site[1], *end = test + 4, *again = end - 13;
    if (again < site + 2 || again[0] != 0xff || again[1] != 0x85 || again[6] != 0x48 || again[7] != 0x8b ||
        test[0] != 0x39 || end[-2] != 0x78 || end[-1] != 0xf3)
        return 0;
    const unsigned load = again[8], compare = test[1];
    /* mov r64, [r64] without SIB or rip; cmp r/m32, r32 on the loaded register */
    if (load >> 6 || (load & 7) == 4 || (load & 7) == 5 || compare >> 6 != 3 || (compare & 7) != (load >> 3 & 7))
        return 0;
    loop->site = site;
    loop->end = end;
    loop->label = load & 7;
    loop->value = load >> 3 & 7;
    loop->target = compare >> 3 & 7;
    /* the stub reads the operands from the registers it saved: not rsp or rbp */
    return loop->target != 4 && loop->target != 5 && loop->value != 4 && loop->value != 5;
}

static ABI void wait_label(const volatile uint64_t *label, uint32_t target) {
    if ((int32_t)((uint32_t)*label - target) >= 0) return;
    mtgpu_wait_label(label, target);
}

/* The stub for `loop`: saves the registers SysV lets a call change, calls wait_label(label, target)
 * on a 16-byte aligned stack, restores them and loads the label as the loop did. Returns its size. */
static size_t emit_stub(unsigned char *code, const Loop *loop) {
    /* push rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11, rbx; their offsets from rbx (= rsp) below */
    static const unsigned char save[] = {0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x53};
    static const unsigned char slot[8] = {72, 64, 56, 0, 0, 0, 48, 40}; /* rax rcx rdx rbx - - rsi rdi */
    static const unsigned char align[] = {0x48, 0x89, 0xe3, 0x48, 0x83, 0xe4, 0xf0}; /* mov rbx, rsp; and rsp, -16 */
    static const unsigned char restore[] = {0x48, 0x89, 0xdc, /* mov rsp, rbx */
                                            0x5b, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58};
    const uintptr_t function = (uintptr_t)wait_label;
    size_t n = 0;
    memcpy(code + n, save, sizeof(save)); n += sizeof(save);
    memcpy(code + n, align, sizeof(align)); n += sizeof(align);
    code[n++] = 0x48; code[n++] = 0x8b; code[n++] = 0x7b; code[n++] = slot[loop->label]; /* mov rdi, [rbx + label] */
    code[n++] = 0x8b; code[n++] = 0x73; code[n++] = slot[loop->target];                 /* mov esi, [rbx + target] */
    code[n++] = 0x48; code[n++] = 0xb8; memcpy(code + n, &function, 8); n += 8;          /* movabs rax, wait_label */
    code[n++] = 0xff; code[n++] = 0xd0;                                                  /* call rax */
    memcpy(code + n, restore, sizeof(restore)); n += sizeof(restore);
    code[n++] = 0x48; code[n++] = 0x8b; code[n++] = (unsigned char)(loop->value << 3 | loop->label); /* mov value, [label] */
    code[n++] = 0xc3;
    return n;
}

unsigned runtime_label_wait_install(unsigned char *image, uint64_t size, unsigned char *stubs, size_t bytes) {
    const char *setting = getenv("MT_LABEL_WAIT");
    if (setting && setting[0] == '0') {
        puts("GPU waits: the render thread spins on the GPU's labels (MT_LABEL_WAIT=0)");
        return 0;
    }
    Loop found[LOOPS];
    unsigned count = 0;
    for (unsigned i = 0; i < LOOPS; ++i) {
        const uint64_t at = mt_signature_find(image, size, &loops[i]);
        if (at != UINT64_MAX && decode(image + at, &found[count])) ++count;
    }
    unsigned replaced = 0;
    for (unsigned i = 0; i < count && (size_t)(replaced + 1) * STUB_BYTES <= bytes; ++i) {
        const Loop *loop = &found[i];
        unsigned char *stub = stubs + (size_t)replaced * STUB_BYTES;
        const int64_t distance = (int64_t)(stub - (loop->site + 5));
        if (distance != (int32_t)distance) continue;
        emit_stub(stub, loop);
        /* call stub; jmp past the loop; int3 over the rest */
        const int32_t rel = (int32_t)distance;
        loop->site[0] = 0xe8;
        memcpy(loop->site + 1, &rel, 4);
        loop->site[5] = 0xeb;
        loop->site[6] = (unsigned char)(loop->end - (loop->site + 7));
        memset(loop->site + 7, 0xcc, (size_t)(loop->end - (loop->site + 7)));
        ++replaced;
    }
    if (replaced) printf("GPU waits: the render thread sleeps until the GPU writes its label (%u loops)\n", replaced);
    else puts("GPU waits: the render thread's loops were not found in this game build: it spins on the GPU's labels");
    return replaced;
}
