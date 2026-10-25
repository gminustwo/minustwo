/* macOS counterparts of the Linux calls the runtime uses. minustwo is an x86-64 program there (the
 * game's code runs directly on the CPU), so on Apple silicon it runs under Rosetta 2. Included
 * through runtime.h, so the macros below only replace calls in this project's own sources;
 * compat_mac.c has the functions. */
#ifndef MT_COMPAT_MAC_H
#define MT_COMPAT_MAC_H
#ifdef __APPLE__
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif
/* struct stat names its times st_atimespec, st_mtimespec and st_ctimespec. */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

/* Condition variables only wait on CLOCK_REALTIME: there is no pthread_condattr_setclock. */
static inline int compat_condattr_setclock(pthread_condattr_t *attr, clockid_t clock) {
    (void)attr;
    return clock == CLOCK_REALTIME ? 0 : EINVAL;
}
#define pthread_condattr_setclock compat_condattr_setclock
/* Timed mutex and rwlock locks, which macOS lacks: the lock is tried until the CLOCK_REALTIME
 * deadline passes (ETIMEDOUT). */
int compat_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline);
int compat_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *deadline);
int compat_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *deadline);
#define pthread_mutex_timedlock compat_mutex_timedlock
#define pthread_rwlock_timedrdlock compat_rwlock_timedrdlock
#define pthread_rwlock_timedwrlock compat_rwlock_timedwrlock

/* Sleeps ns nanoseconds (no clock_nanosleep: absolute sleeps loop on this, as on Windows). */
void compat_sleep_ns(uint64_t ns);
/* User and kernel CPU time of the process (who=0) or calling thread (who=1), microseconds
 * (there is no RUSAGE_THREAD). */
int compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us);
/* System-wide id of the calling thread (gettid). */
uint64_t compat_thread_id(void);
/* Path of the running executable; 0 or -1. */
int compat_executable_path(char *out, size_t size);
/* Copies size bytes from address without faulting (frame walks); 1 when all were readable. */
int compat_read_memory(uintptr_t address, void *out, size_t size);
/* Sends sig to every other thread of the process, pausing gap_us after each. */
void compat_signal_threads(int sig, unsigned gap_us);
/* Calls each(context, name, cpu_ms) for every live thread of the process. */
void compat_each_thread(void (*each)(void *context, const char *name, double cpu_ms), void *context);
/* Under Rosetta 2, starts the program again (argv) with ROSETTA_ADVERTISE_AVX=1 unless it has it:
 * Rosetta runs AVX code (macOS 15 and newer) but reports AVX, F16C, BMI and MOVBE in CPUID only
 * with that variable, read when the process starts. Returns when nothing needs to change. */
void compat_rosetta_avx(char **argv);
#endif
#endif
