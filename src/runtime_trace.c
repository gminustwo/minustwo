/* MT_IMPORT_TRACE=<prefix>[,<prefix>...] ("*" for all): every import whose symbol starts with a
 * prefix (sceNp, sceNet, sceRudp, ...) goes through a generated trampoline that prints its name, first
 * four arguments and the guest caller, then what it returns: which calls a game makes, in which order
 * and with what (reverse engineering, e.g. the online modes). The return is caught by replacing the
 * return address (a per-thread shadow stack keeps the real ones), so stack arguments stay where they
 * are. Not for functions that do not return to their caller (thread exit, longjmp). */
#define _GNU_SOURCE
#include "runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

enum { MAX_TRACED = 4096, TRAMPOLINE_SIZE = 160, SHADOW_DEPTH = 256 };
static char *names[MAX_TRACED];
static uint32_t traced;
static unsigned char *code, *return_thunk;
static size_t code_used, code_size;
typedef struct { uint32_t id; uint64_t caller; } ShadowEntry;
static _Thread_local ShadowEntry shadow[SHADOW_DEPTH];
static _Thread_local int shadow_depth;

/* regs: rax, r9, r8, rcx, rdx, rsi, rdi as the trampoline pushed them, then the return address. */
static ABI void trace_enter(uint32_t id, uint64_t *regs) {
    printf("Trace: %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) from 0x%llx\n", names[id], (unsigned long long)regs[6],
           (unsigned long long)regs[5], (unsigned long long)regs[4], (unsigned long long)regs[3],
           (unsigned long long)regs[7]);
    fflush(stdout);
    if (shadow_depth < SHADOW_DEPTH) {
        shadow[shadow_depth++] = (ShadowEntry){id, regs[7]};
        regs[7] = (uint64_t)(uintptr_t)return_thunk;
    }
}

/* The traced function returned `value`: prints it, gives back the real return address. */
static ABI uint64_t trace_leave(uint64_t value) {
    ShadowEntry entry = shadow[--shadow_depth];
    printf("Trace: %s -> 0x%llx\n", names[entry.id], (unsigned long long)value);
    fflush(stdout);
    return entry.caller;
}

static unsigned char *emit(unsigned char *p, const void *bytes, size_t n) { memcpy(p, bytes, n); return p + n; }
static unsigned char *emit_u32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }
static unsigned char *emit_u64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); return p + 8; }

/* movdqu [rsp+8*2*i], xmm_i (store) or the reverse (load) for xmm0..7. */
static unsigned char *xmm_block(unsigned char *p, int store) {
    for (int i = 0; i < 8; ++i) {
        unsigned char op[6] = {0xF3, 0x0F, (unsigned char)(store ? 0x7F : 0x6F), 0, 0x24, 0};
        if (i == 0) {
            op[3] = 0x04; /* [rsp] */
            p = emit(p, op, 5);
        } else {
            op[3] = (unsigned char)(0x44 | (i << 3)); /* [rsp+disp8] */
            op[5] = (unsigned char)(i * 16);
            p = emit(p, op, 6);
        }
    }
    return p;
}

static int allocate_code(void) {
    code_size = (size_t)MAX_TRACED * TRAMPOLINE_SIZE + 4096;
#ifdef _WIN32
    code = VirtualAlloc(NULL, code_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    code = mmap(NULL, code_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) code = NULL;
#endif
    if (!code) return 0;
    /* The return thunk: the traced function returned here with rax/rdx (and xmm0) as its result. */
    unsigned char *p = code;
    p = emit(p, "\x48\x83\xEC\x08", 4);             /* sub rsp, 8: the slot for the real return address */
    p = emit(p, "\x50\x52", 2);                     /* push rax; push rdx */
    p = emit(p, "\x48\x83\xEC\x18", 4);             /* sub rsp, 0x18 (16-byte aligned for the call) */
    p = emit(p, "\xF3\x0F\x7F\x04\x24", 5);         /* movdqu [rsp], xmm0 */
    p = emit(p, "\x48\x89\xC7", 3);                 /* mov rdi, rax */
    p = emit(p, "\x49\xBB", 2);                     /* mov r11, trace_leave */
    p = emit_u64(p, (uint64_t)(uintptr_t)trace_leave);
    p = emit(p, "\x41\xFF\xD3", 3);                 /* call r11 */
    p = emit(p, "\x48\x89\x44\x24\x28", 5);         /* mov [rsp+0x28], rax: the real return address */
    p = emit(p, "\xF3\x0F\x6F\x04\x24", 5);         /* movdqu xmm0, [rsp] */
    p = emit(p, "\x48\x83\xC4\x18", 4);             /* add rsp, 0x18 */
    p = emit(p, "\x5A\x58\xC3", 3);                 /* pop rdx; pop rax; ret */
    return_thunk = code;
    code_used = 64;
    return 1;
}

static int wanted(const char *symbol) {
    static const char *spec;
    static int read;
    if (!read) { spec = getenv("MT_IMPORT_TRACE"); read = 1; }
    if (!spec || !*spec || !symbol) return 0;
    for (const char *s = spec; *s;) {
        const char *end = strchr(s, ',');
        size_t n = end ? (size_t)(end - s) : strlen(s);
        if ((n == 1 && *s == '*') || (n && !strncmp(symbol, s, n))) return 1;
        if (!end) break;
        s = end + 1;
    }
    return 0;
}

uintptr_t runtime_trace_wrap(const char *symbol, uintptr_t target) {
    if (!target || !wanted(symbol) || traced >= MAX_TRACED) return target;
    if (!code && !allocate_code()) return target;
    uint32_t id = traced++;
    names[id] = strdup(symbol);
    unsigned char *start = code + code_used, *p = start;
    /* Entry: save the argument registers (rax: the vector count of variadic calls), log, restore,
     * jump on through r11 (no argument lives there). 7 pushes + 0x80 keep the call 16-byte aligned. */
    p = emit(p, "\x57\x56\x52\x51\x41\x50\x41\x51\x50", 9); /* push rdi, rsi, rdx, rcx, r8, r9, rax */
    p = emit(p, "\x48\x83\xC4\x80", 4);                     /* add rsp, -0x80 */
    p = xmm_block(p, 1);
    p = emit(p, "\xBF", 1);                                 /* mov edi, id */
    p = emit_u32(p, id);
    p = emit(p, "\x48\x8D\xB4\x24\x80\x00\x00\x00", 8);     /* lea rsi, [rsp+0x80]: the pushed registers */
    p = emit(p, "\x49\xBB", 2);                             /* mov r11, trace_enter */
    p = emit_u64(p, (uint64_t)(uintptr_t)trace_enter);
    p = emit(p, "\x41\xFF\xD3", 3);                         /* call r11 */
    p = xmm_block(p, 0);
    p = emit(p, "\x48\x83\xEC\x80", 4);                     /* sub rsp, -0x80 */
    p = emit(p, "\x58\x41\x59\x41\x58\x59\x5A\x5E\x5F", 9); /* pop rax, r9, r8, rcx, rdx, rsi, rdi */
    p = emit(p, "\x49\xBB", 2);                             /* mov r11, target */
    p = emit_u64(p, (uint64_t)target);
    p = emit(p, "\x41\xFF\xE3", 3);                         /* jmp r11 */
    code_used += TRAMPOLINE_SIZE;
    if ((size_t)(p - start) > TRAMPOLINE_SIZE) abort(); /* the layout above outgrew its slot */
    return (uintptr_t)start;
}
