// Whether a call that wakes a higher priority waiter on the same core switches to it before the
// call returns, for every kind of wake: a semaphore signal, a mutex and a lightweight mutex unlock,
// an event flag and a simple event set, a rwlock unlock, a message pipe send, a thread signal, a
// semaphore cancel and delete, and a callback notified to a thread in a delay with callbacks. Also
// a condition and a lightweight condition signal, whose waiter still needs the mutex the signaler
// holds, and the unlock after it. Both threads are pinned to core 0 and the waiter is above the
// main thread. The main thread records 0 before the call and 2 after it (3 after the unlock that
// follows a condition signal), the waiter records 1 when its wait returns or its callback runs.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0xA0
#define WAITER_PRIORITY 0x80
#define MAX_STEPS 6

typedef enum Kind { SEMA, MUTEX, LW_MUTEX, FLAG, EVENT, RWLOCK, PIPE, SIGNAL, CANCEL, DELETE, CALLBACK, COND, LW_COND } Kind;

static Kind kind;
static SceUID object, mutex, callback;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int result, callback_ready;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    record(1);
    return 0;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    SceUInt32 bits;
    char buf[4];
    switch (kind) {
    case SEMA:
    case CANCEL:
    case DELETE: result = sceKernelWaitSema(object, 1, &timeout); break;
    case MUTEX:
        result = sceKernelLockMutex(object, 1, &timeout);
        if (result == 0)
            sceKernelUnlockMutex(object, 1);
        break;
    case LW_MUTEX:
        result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        if (result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case FLAG: result = sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case EVENT: result = sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout); break;
    case RWLOCK:
        result = sceKernelLockWriteRWLock(object, &timeout);
        if (result == 0)
            sceKernelUnlockWriteRWLock(object);
        break;
    case PIPE: result = sceKernelReceiveMsgPipe(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    case SIGNAL: result = sceKernelWaitSignal(0, 0, &timeout); break;
    case CALLBACK:
        callback = sceKernelCreateCallback("cb", 0, callback_entry, NULL);
        callback_ready = 1;
        result = sceKernelDelayThreadCB(300000);
        sceKernelDeleteCallback(callback);
        return 0;
    case COND:
        sceKernelLockMutex(mutex, 1, NULL);
        result = sceKernelWaitCond(object, &timeout);
        sceKernelUnlockMutex(mutex, 1);
        break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        result = sceKernelWaitLwCond(&lw_cond, &timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    }
    record(1);
    return 0;
}

static void begin(Kind k) {
    kind = k;
    result = 0xDEAD;
    callback_ready = 0;
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
}

// Starts the waiter, which runs at once since it is above the main thread, until it waits
static SceUID start_waiter(void) {
    const SceUID thread = sceKernelCreateThread("waiter", waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    test_wait_until_waiting(thread);
    return thread;
}

static void finish(const char *name, SceUID thread, int call) {
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    sceKernelDeleteThread(thread);
    PRINTF(call, "%s: the call", name);
    PRINTF(result, "%s: the waiter's result", name);
    const int count = step_count < MAX_STEPS ? step_count : MAX_STEPS;
    for (int i = 0; i < count; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
}

static void simple_case(const char *name, Kind k) {
    begin(k);
    switch (k) {
    case SEMA:
    case CANCEL:
    case DELETE: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case MUTEX: object = sceKernelCreateMutex("mutex", 0, 1, NULL); break;
    case LW_MUTEX: sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL); break;
    case RWLOCK:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    default: break;
    }
    const SceUID thread = start_waiter();
    if (k == CALLBACK)
        TEST_WAIT_FOR(callback_ready);
    int call = 0;
    SceSize size;
    int waiters;
    record(0);
    switch (k) {
    case SEMA: call = sceKernelSignalSema(object, 1); break;
    case MUTEX: call = sceKernelUnlockMutex(object, 1); break;
    case LW_MUTEX: call = sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case FLAG: call = sceKernelSetEventFlag(object, 0x1); break;
    case EVENT: call = sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    case RWLOCK: call = sceKernelUnlockWriteRWLock(object); break;
    case PIPE: call = sceKernelTrySendMsgPipe(object, "abcd", 4, MSG_PIPE_MODE_FULL, &size); break;
    case SIGNAL: call = sceKernelSendSignal(thread); break;
    case CANCEL: call = sceKernelCancelSema(object, 0, &waiters); break;
    case DELETE: call = sceKernelDeleteSema(object); break;
    case CALLBACK: call = sceKernelNotifyCallback(callback, 1); break;
    default: break;
    }
    record(2);
    finish(name, thread, call);
    switch (k) {
    case SEMA:
    case CANCEL: sceKernelDeleteSema(object); break;
    case MUTEX: sceKernelDeleteMutex(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case RWLOCK: sceKernelDeleteRWLock(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    default: break;
    }
}

static void cond_case(const char *name, Kind k) {
    begin(k);
    if (k == COND) {
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
        object = sceKernelCreateCond("cond", 0, mutex, NULL);
    } else {
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lwcond", 0, &lw_mutex, NULL);
    }
    const SceUID thread = start_waiter();
    if (k == COND)
        sceKernelLockMutex(mutex, 1, NULL);
    else
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    record(0);
    const int call = k == COND ? sceKernelSignalCond(object) : sceKernelSignalLwCond(&lw_cond);
    record(2);
    if (k == COND)
        sceKernelUnlockMutex(mutex, 1);
    else
        sceKernelUnlockLwMutex(&lw_mutex, 1);
    record(3);
    finish(name, thread, call);
    if (k == COND) {
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(mutex);
    } else {
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_mutex);
    }
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    simple_case("semaphore signal", SEMA);
    simple_case("mutex unlock", MUTEX);
    simple_case("lightweight mutex unlock", LW_MUTEX);
    simple_case("event flag set", FLAG);
    simple_case("simple event set", EVENT);
    simple_case("rwlock write unlock", RWLOCK);
    simple_case("message pipe send", PIPE);
    simple_case("thread signal", SIGNAL);
    simple_case("semaphore cancel", CANCEL);
    simple_case("semaphore delete", DELETE);
    cond_case("condition signal, then the mutex unlock", COND);
    cond_case("lightweight condition signal, then the mutex unlock", LW_COND);
    simple_case("callback notify to a thread in a delay with callbacks", CALLBACK);

    test_exit();
    return 0;
}
