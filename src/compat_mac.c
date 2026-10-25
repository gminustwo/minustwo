/* macOS counterparts of the Linux calls the runtime uses (see compat_mac.h). */
#include "runtime.h"
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>

static int deadline_passed(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}
/* Tries the lock until it is free or the deadline passes: yields first, then sleeps up to 1 ms. */
#define TIMED_LOCK(try_lock, deadline)                                                           \
    do {                                                                                         \
        if ((deadline)->tv_nsec < 0 || (deadline)->tv_nsec >= 1000000000) return EINVAL;        \
        for (unsigned attempt = 0;; ++attempt) {                                                 \
            int e = (try_lock);                                                                  \
            if (e != EBUSY) return e;                                                            \
            if (deadline_passed(deadline)) return ETIMEDOUT;                                     \
            if (attempt < 64) sched_yield();                                                     \
            else compat_sleep_ns(attempt < 256 ? 50000 : 1000000);                               \
        }                                                                                        \
    } while (0)
int compat_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline) {
    TIMED_LOCK(pthread_mutex_trylock(mutex), deadline);
}
int compat_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *deadline) {
    TIMED_LOCK(pthread_rwlock_tryrdlock(lock), deadline);
}
int compat_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *deadline) {
    TIMED_LOCK(pthread_rwlock_trywrlock(lock), deadline);
}
void compat_sleep_ns(uint64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
int compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us) {
    if (who == 0) {
        struct rusage r;
        if (getrusage(RUSAGE_SELF, &r)) return -1;
        *user_us = (int64_t)r.ru_utime.tv_sec * 1000000 + r.ru_utime.tv_usec;
        *kernel_us = (int64_t)r.ru_stime.tv_sec * 1000000 + r.ru_stime.tv_usec;
        return 0;
    }
    mach_port_t thread = mach_thread_self();
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    kern_return_t result = thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), thread);
    if (result != KERN_SUCCESS) return -1;
    *user_us = (int64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds;
    *kernel_us = (int64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
    return 0;
}
uint64_t compat_thread_id(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return id;
}
int compat_executable_path(char *out, size_t size) {
    char raw[PATH_MAX], resolved[PATH_MAX];
    uint32_t raw_length = sizeof(raw);
    if (_NSGetExecutablePath(raw, &raw_length)) return -1;
    const char *path = realpath(raw, resolved) ? resolved : raw;
    if (strlen(path) >= size) return -1;
    strcpy(out, path);
    return 0;
}
int compat_read_memory(uintptr_t address, void *out, size_t size) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)(uintptr_t)out, &got) ==
               KERN_SUCCESS && got == size;
}
void compat_signal_threads(int sig, unsigned gap_us) {
    thread_act_array_t threads;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
    mach_port_t self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        pthread_t thread = threads[i] != self ? pthread_from_mach_thread_np(threads[i]) : NULL;
        if (thread) { pthread_kill(thread, sig); usleep(gap_us); }
    }
    /* Ports are left to the process's end: this runs in a signal handler that then exits. */
}
void compat_each_thread(void (*each)(void *context, const char *name, double cpu_ms), void *context) {
    thread_act_array_t threads;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        thread_extended_info_data_t info;
        mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, (thread_info_t)&info, &size) == KERN_SUCCESS) {
            char name[64];
            if (info.pth_name[0]) snprintf(name, sizeof(name), "%s", info.pth_name);
            else snprintf(name, sizeof(name), "thread %u", (unsigned)threads[i]);
            each(context, name, (double)(info.pth_user_time + info.pth_system_time) / 1e6);
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(*threads));
}
void compat_rosetta_avx(char **argv) {
    int translated = 0;
    size_t size = sizeof(translated);
    if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) || !translated) return;
    const char *advertise = getenv("ROSETTA_ADVERTISE_AVX");
    char self[PATH_MAX];
    if ((advertise && !strcmp(advertise, "1")) || compat_executable_path(self, sizeof(self))) return;
    setenv("ROSETTA_ADVERTISE_AVX", "1", 1);
    execv(self, argv);
    perror("compat_rosetta_avx: execv");
}
