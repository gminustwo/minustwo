/* The render thread's GPU waits (src/runtime_label_wait.c): the game's four spin loops on the
 * GPU's label (their bytes as in 01.01 and 01.00) are found, replaced by calls to stubs, and the
 * patched code runs: it waits only while the label is short of the fence value, loads the label
 * into the loop's register and keeps every other register. */
#define _GNU_SOURCE /* MAP_ANONYMOUS under -std=c11 */
#undef NDEBUG       /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_label_wait.c"
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static const volatile void *waited_label;
static uint32_t waited_target;
static unsigned waits;
/* The GPU library's wait: the GPU "writes" the label at once. */
void mtgpu_wait_label(const volatile void *label, uint32_t target) {
    waited_label = label;
    waited_target = target;
    ++waits;
    *(volatile uint64_t *)label = target;
}

/* Runs the patched site with these registers; out: rdi, rdx, rsi, rcx, rax after the loop
 * (stored by the code placed after it through r8). rax comes from r9 through a prologue. */
typedef void (ABI *Run)(uint64_t rdi, uint64_t rsi, uint64_t rdx, uint64_t rcx, uint64_t *out, uint64_t rax);
enum { PAGE = 4096, IMAGE = 4 * PAGE, LOOP_AT = 0x100, PROLOGUE_AT = 0x800 };
static const unsigned char after[] = {0x49, 0x89, 0x38,       /* mov [r8], rdi */
                                      0x49, 0x89, 0x50, 0x08, /* mov [r8 + 8], rdx */
                                      0x49, 0x89, 0x70, 0x10, /* mov [r8 + 16], rsi */
                                      0x49, 0x89, 0x48, 0x18, /* mov [r8 + 24], rcx */
                                      0x49, 0x89, 0x40, 0x20, /* mov [r8 + 32], rax */
                                      0xc3};

static unsigned char *region;
static Run entries[LOOPS];

/* Each loop's bytes at LOOP_AT + i * 0x100 (the turn count at [rbp - 0xac] for "??"), the
 * register stores after it, and a prologue that moves r9 to rax and jumps to its site. */
static void place_loops(void) {
    for (unsigned i = 0; i < LOOPS; ++i) {
        int pattern[MT_SIGNATURE_MAX];
        const unsigned n = mt_signature_parse(loops[i].pattern, pattern);
        assert(n);
        static const unsigned char turns[] = {0x54, 0xff, 0xff, 0xff};
        unsigned char *at = region + LOOP_AT + i * 0x100;
        for (unsigned k = 0, unknown = 0; k < n; ++k) at[k] = pattern[k] < 0 ? turns[unknown++ % 4] : (unsigned char)pattern[k];
        memcpy(at + n, after, sizeof(after));
        unsigned char *prologue = region + PROLOGUE_AT + i * 0x20;
        const int32_t rel = (int32_t)((at + loops[i].at) - (prologue + 8));
        memcpy(prologue, (const unsigned char[]){0x4c, 0x89, 0xc8, 0xe9}, 4); /* mov rax, r9; jmp site */
        memcpy(prologue + 4, &rel, 4);
        entries[i] = (Run)(void *)prologue;
    }
}

int main(void) {
#ifdef _WIN32
    region = VirtualAlloc(NULL, IMAGE + PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(region);
#else
    region = mmap(NULL, IMAGE + PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region != MAP_FAILED);
#endif
    unsigned char *stubs = region + IMAGE;
    assert(runtime_label_wait_install(region, IMAGE, stubs, PAGE) == 0); /* no loops: nothing changed */
    place_loops();
    assert(runtime_label_wait_install(region, IMAGE, stubs, STUB_BYTES * 2) == 2); /* stub room for two */
    memset(region, 0, IMAGE);
    memset(stubs, 0, PAGE);
    place_loops();
    assert(runtime_label_wait_install(region, IMAGE, stubs, PAGE) == LOOPS);

    for (unsigned i = 0; i < LOOPS; ++i) {
        /* call stub; jmp past the loop; int3 over the rest of it */
        const MtSignature *sig = &loops[i];
        unsigned char *site = region + LOOP_AT + i * 0x100 + sig->at;
        int pattern[MT_SIGNATURE_MAX];
        unsigned char *end = region + LOOP_AT + i * 0x100 + mt_signature_parse(sig->pattern, pattern);
        assert(site[0] == 0xe8 && site[5] == 0xeb && site + 7 + site[6] == end);
        for (unsigned char *p = site + 7; p < end; ++p) assert(*p == 0xcc);
        int32_t rel;
        memcpy(&rel, site + 1, 4);
        assert(site + 5 + rel == stubs + i * STUB_BYTES);
    }

    /* Loops 1 and 2 wait on [rcx] for esi and leave the label in rdi; 0 and 3 on [rax] for ecx,
     * leaving it in rdx. */
    for (unsigned i = 0; i < LOOPS; ++i) {
        const int in_rcx = i == 1 || i == 2;
        static const struct { uint64_t label; uint32_t target; int waits; } cases[] = {
            {5, 7, 1}, {9, 7, 0}, {7, 7, 0},
            {0xfffffff0, 0x10, 1},          /* the count wrapped past the label: still short */
            {0x10, 0xfffffff0, 0},          /* the label wrapped past the target: reached */
            {0x500000009, 7, 0},            /* only the low 32 bits count */
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); ++c) {
            volatile uint64_t label = cases[c].label;
            uint64_t out[5] = {0};
            const uint64_t pointer = (uint64_t)(uintptr_t)&label;
            const unsigned before = waits;
            if (in_rcx)
                entries[i](0x1111, cases[c].target, 0x2222, pointer, out, 0x3333);
            else
                entries[i](0x1111, 0x4444, 0x2222, cases[c].target, out, pointer);
            assert(waits - before == (unsigned)cases[c].waits);
            if (cases[c].waits) assert(waited_label == (const volatile void *)&label && waited_target == cases[c].target);
            const uint64_t now = label;
            assert(cases[c].waits ? (uint32_t)now == cases[c].target : now == cases[c].label);
            if (in_rcx) {
                assert(out[0] == now && out[1] == 0x2222 && out[2] == cases[c].target && out[3] == pointer && out[4] == 0x3333);
            } else {
                assert(out[0] == 0x1111 && out[1] == now && out[2] == 0x4444 && out[3] == cases[c].target && out[4] == pointer);
            }
        }
    }
    puts("label wait: 4 loops replaced; waits, loaded label and registers as the game's loops");
    return 0;
}
