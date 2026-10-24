/* Kernel event flags (src/runtime_eventflag.c): wait modes, clearing, polling, timeouts, deletion. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_eventflag.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }

static uint64_t flag;
static void *setter(void *arg) {
    (void)arg;
    struct timespec t={0,20000000};
    nanosleep(&t,NULL);
    flag_set(flag,0x6);
    return NULL;
}

int main(void) {
    uint64_t result=0;
    uint32_t timeout;
    assert(flag_create(&flag,"fsys_eflag",ATTR_TH_FIFO|ATTR_SINGLE,0x1,NULL)==0 && flag);
    assert(flag_create(&flag,NULL,0,0,NULL)==ERR(22));
    assert(flag_create(&result,"x",0x40,0,NULL)==ERR(22));

    /* OR is satisfied by any bit; AND needs all of them. */
    assert(flag_poll(flag,0x3,WAIT_OR,&result)==0 && result==0x1);
    assert(flag_poll(flag,0x3,WAIT_AND,&result)==ERR(16) && result==0x1);
    assert(flag_poll(flag,0,WAIT_OR,&result)==ERR(22) && flag_poll(flag,1,0x3,&result)==ERR(22));

    /* Clearing keeps the bits given (the PS4's AND), the wait modes clear all or the pattern. */
    assert(flag_set(flag,0xf0)==0 && flag_clear(flag,0xf1)==0);
    assert(flag_poll(flag,0x10,WAIT_AND|WAIT_CLEAR_PAT,&result)==0 && result==0xf1);
    assert(flag_poll(flag,0xff,WAIT_OR,&result)==0 && result==0xe1);
    assert(flag_poll(flag,0x1,WAIT_OR|WAIT_CLEAR_ALL,&result)==0 && result==0xe1);
    assert(flag_poll(flag,0xff,WAIT_OR,&result)==ERR(16) && result==0);

    /* A timed wait that runs out reports the time left as 0 and the bits as they are. */
    timeout=1000;
    assert(flag_wait(flag,0x1,WAIT_OR,&result,&timeout)==ERR(60) && timeout==0 && result==0);

    /* Another thread's set wakes the waiter once all its bits are there. */
    pthread_t thread;
    pthread_create(&thread,NULL,setter,NULL);
    timeout=2000000;
    assert(flag_wait(flag,0x6,WAIT_AND|WAIT_CLEAR_PAT,&result,&timeout)==0 && result==0x6 && timeout>0);
    pthread_join(thread,NULL);
    assert(flag_poll(flag,0x6,WAIT_OR,&result)==ERR(16));

    /* Cancel sets the pattern; a deleted flag is gone. */
    int32_t waiters=-1;
    assert(flag_cancel(flag,0x8,&waiters)==0 && waiters==0);
    assert(flag_poll(flag,0x8,WAIT_OR,&result)==0);
    assert(flag_delete(flag)==0 && flag_delete(flag)==ERR(3) && flag_set(flag,1)==ERR(3));
    puts("event flag test: ok");
    return 0;
}
