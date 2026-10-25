// SPDX-License-Identifier: GPL-2.0-or-later
// minustwo (macOS): the thread identity helper the renderer uses (Linux's gettid). Force-included
// into the GPU library on macOS only (CMakeLists.txt). The 64-bit thread id is cut to the 32 bits
// the callers store: ids are allocated in sequence, so they stay distinct.
#pragma once
#ifdef __APPLE__
#include <pthread.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline int gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (int)id;
}
#ifdef __cplusplus
}
#endif
#endif
