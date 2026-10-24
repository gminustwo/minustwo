/* Kernel event flags: a 64-bit pattern that threads wait on (all or any of their bits), with
 * optional clearing when a wait is satisfied. The handle the guest keeps is an opaque id, not a
 * host pointer. Priority-ordered flags are served FIFO, as the semaphores are (runtime_sema.c). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_TH_FIFO 0x01
#define ATTR_TH_PRIO 0x02
#define ATTR_SINGLE 0x10
#define ATTR_MULTI 0x20
#define WAIT_AND 0x01
#define WAIT_OR 0x02
#define WAIT_CLEAR_ALL 0x10
#define WAIT_CLEAR_PAT 0x20
typedef struct Waiter {
    uint64_t bits, result;
    uint32_t mode;
    int32_t status; /* 0 satisfied, else the error the wait returns */
    int done;
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Flag {
    uint64_t id, bits;
    int multi, deleted;
    unsigned active;
    char name[32];
    Waiter *first;
    struct Flag *next;
} Flag;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Flag *registry;
static uint64_t next_id=1;
static size_t created, deleted, waits, sets, timed_out;
static Flag *find(uint64_t id) {
    for (Flag *f=registry;f;f=f->next) if (f->id==id) return f;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: event flag host error %d\n",e); exit(21); }
}
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static int satisfied(const Flag *f, uint64_t bits, uint32_t mode) {
    return (mode & WAIT_AND) ? (f->bits & bits)==bits : (f->bits & bits)!=0;
}
static void apply_clear(Flag *f, uint64_t bits, uint32_t mode) {
    if (mode & WAIT_CLEAR_ALL) f->bits=0;
    else if (mode & WAIT_CLEAR_PAT) f->bits&=~bits;
}
static int valid_mode(uint32_t mode) {
    const uint32_t wait=mode & 0xf, clear=mode & 0xf0;
    return (wait==WAIT_AND || wait==WAIT_OR) && (clear==0 || clear==WAIT_CLEAR_ALL || clear==WAIT_CLEAR_PAT) && !(mode & ~0xffu);
}
static ABI int32_t flag_create(uint64_t *out, const char *name, uint32_t attr, uint64_t pattern, const void *options) {
    if (!out || !name || options) return ERR(22);
    if ((attr & 0xf)>ATTR_TH_PRIO || ((attr & 0xf0) && (attr & 0xf0)!=ATTR_SINGLE && (attr & 0xf0)!=ATTR_MULTI) || (attr & ~0xffu))
        return ERR(22);
    if (strlen(name)>=32) return ERR(63); /* ENAMETOOLONG */
    Flag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    pthread_mutex_lock(&lock);
    f->id=next_id++; f->bits=pattern; f->multi=(attr & 0xf0)==ATTR_MULTI;
    snprintf(f->name,sizeof(f->name),"%s",name);
    f->next=registry; registry=f; *out=f->id; ++created;
    printf("Runtime: event flag id=%llu '%s', attr=0x%x, pattern=0x%llx\n",(unsigned long long)f->id,name,attr,
           (unsigned long long)pattern);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t flag_delete(uint64_t id) {
    pthread_mutex_lock(&lock);
    Flag **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Flag *f=*p; *p=f->next; f->deleted=1; ++deleted;
    while (f->first) {
        Waiter *w=f->first; f->first=w->next;
        w->status=ERR(13); w->result=f->bits; w->done=1;
        host_check(pthread_cond_signal(&w->event));
    }
    if (!f->active) free(f);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t flag_set(uint64_t id, uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    f->bits|=bits; ++sets;
    Waiter **p=&f->first;
    while (*p) {
        Waiter *w=*p;
        if (!satisfied(f,w->bits,w->mode)) { p=&w->next; continue; }
        w->result=f->bits;
        apply_clear(f,w->bits,w->mode);
        w->status=0; w->done=1; *p=w->next;
        host_check(pthread_cond_signal(&w->event));
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t flag_clear(uint64_t id, uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (f) f->bits&=bits; /* the bits to keep, as on the PS4 */
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static ABI int32_t flag_cancel(uint64_t id, uint64_t pattern, int32_t *waiters) {
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    int32_t n=0;
    f->bits=pattern;
    while (f->first) {
        Waiter *w=f->first; f->first=w->next;
        w->status=ERR(85); w->result=pattern; w->done=1; ++n;
        host_check(pthread_cond_signal(&w->event));
    }
    if (waiters) *waiters=n;
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t wait_flag(uint64_t id, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout, int block) {
    if (!bits || !valid_mode(mode)) return ERR(22);
    pthread_mutex_lock(&lock);
    Flag *f=find(id);
    int32_t status=0;
    if (!f) status=ERR(3);
    else if (!f->multi && f->first) status=ERR(1); /* a single-waiter flag already has its waiter */
    else if (satisfied(f,bits,mode)) {
        if (result) *result=f->bits;
        apply_clear(f,bits,mode);
    } else if (!block) { status=ERR(16); if (result) *result=f->bits; }
    else if (timeout && !*timeout) { status=ERR(60); ++timed_out; if (result) *result=f->bits; }
    else {
        Waiter w={.bits=bits,.mode=mode};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&f->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++f->active; ++waits;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        while (!w.done) {
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&f->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.status=ERR(60); w.result=f->bits; w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        status=w.status;
        if (result) *result=w.result;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=status==ERR(60) || now>=deadline ? 0 : (uint32_t)((deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--f->active && f->deleted) free(f);
    }
    pthread_mutex_unlock(&lock);
    return status;
}
static ABI int32_t flag_wait(uint64_t id, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout) {
    return wait_flag(id,bits,mode,result,timeout,1);
}
static ABI int32_t flag_poll(uint64_t id, uint64_t bits, uint32_t mode, uint64_t *result) {
    return wait_flag(id,bits,mode,result,NULL,0);
}
static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",flag_create}, {"sceKernelDeleteEventFlag",flag_delete},
    {"sceKernelSetEventFlag",flag_set}, {"sceKernelClearEventFlag",flag_clear},
    {"sceKernelCancelEventFlag",flag_cancel}, {"sceKernelWaitEventFlag",flag_wait},
    {"sceKernelPollEventFlag",flag_poll},
};
uintptr_t runtime_eventflag_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_eventflag_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: event flags created=%zu, deleted=%zu, sets=%zu, blocking waits=%zu, timeouts=%zu\n",
           created,deleted,sets,waits,timed_out);
    pthread_mutex_unlock(&lock);
}
