// The order in which threads that one call wakes all at once run afterwards: three waiters of one
// priority queue in the order 0, 1, 2 on core 0, below the main thread, which then makes one call
// that ends every wait and only then blocks. Calls: a semaphore signal of 3, cancel and delete, an
// event flag set, cancel and delete, a mutex cancel and delete, a rwlock write unlock with three
// readers, cancel and delete, a manual reset simple event set, cancel and delete, a message pipe
// cancel and delete with three receivers, a lightweight mutex delete, and the end of a thread three
// threads wait for. Each run order prints as one hex digit per waiter, first to run first, next to
// what the first waiter's wait returned.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xA0

typedef enum Kind { SEMA, FLAG, MUTEX, RWLOCK, EVENT, PIPE, LW_MUTEX, THREAD_END } Kind;
typedef enum Call { RELEASE, CANCEL, DELETE } Call;

static Kind kind;
static SceUID object, target;
static SceKernelLwMutexWork lw_mutex;
static volatile int run_count, end_target;
static int order[3];
static volatile int result[3];

static void ran(int i) {
    const int n = __atomic_fetch_add(&run_count, 1, __ATOMIC_SEQ_CST);
    if (n < 3)
        order[n] = i;
}

static int target_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    TEST_WAIT_FOR(end_target);
    return 0;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    SceUInt32 bits;
    char byte;
    switch (kind) {
    case SEMA: result[i] = sceKernelWaitSema(object, 1, &timeout); break;
    case FLAG: result[i] = sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case MUTEX: result[i] = sceKernelLockMutex(object, 1, &timeout); break;
    case RWLOCK: result[i] = sceKernelLockReadRWLock(object, &timeout); break;
    case EVENT: result[i] = sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout); break;
    case PIPE: result[i] = sceKernelReceiveMsgPipe(object, &byte, 1, MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    case LW_MUTEX: result[i] = sceKernelLockLwMutex(&lw_mutex, 1, &timeout); break;
    case THREAD_END: result[i] = sceKernelWaitThreadEnd(target, NULL, &timeout); break;
    }
    ran(i);
    return 0;
}

static void create(Kind k) {
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 3, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", SCE_EVENT_WAITMULTIPLE, 0, NULL); break;
    case MUTEX: object = sceKernelCreateMutex("mutex", 0, 1, NULL); break;
    case RWLOCK:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_MANUAL_RESET, 0, NULL); break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    case LW_MUTEX: sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 1, NULL); break;
    case THREAD_END:
        end_target = 0;
        target = sceKernelCreateThread("target", target_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
        sceKernelStartThread(target, 0, NULL);
        break;
    }
}

static int call(Kind k, Call c) {
    int waiting;
    SceUInt32 waiting_u;
    int senders;
    switch (k) {
    case SEMA: return c == RELEASE ? sceKernelSignalSema(object, 3) : c == CANCEL ? sceKernelCancelSema(object, 0, &waiting) : sceKernelDeleteSema(object);
    case FLAG: return c == RELEASE ? sceKernelSetEventFlag(object, 0x1) : c == CANCEL ? sceKernelCancelEventFlag(object, 0, &waiting_u) : sceKernelDeleteEventFlag(object);
    case MUTEX: return c == CANCEL ? sceKernelCancelMutex(object, 0, &waiting) : sceKernelDeleteMutex(object);
    case RWLOCK: return c == RELEASE ? sceKernelUnlockWriteRWLock(object) : c == CANCEL ? sceKernelCancelRWLock(object, &waiting_u, &waiting_u, 0) : sceKernelDeleteRWLock(object);
    case EVENT: return c == RELEASE ? sceKernelSetEvent(object, EVENT_BIT_A, 0) : c == CANCEL ? sceKernelCancelEvent(object, &waiting_u) : sceKernelDeleteSimpleEvent(object);
    case PIPE: return c == CANCEL ? sceKernelCancelMsgPipe(object, &senders, &waiting) : sceKernelDeleteMsgPipe(object);
    case LW_MUTEX: return sceKernelDeleteLwMutex(&lw_mutex);
    case THREAD_END:
        end_target = 1;
        return sceKernelWaitThreadEnd(target, NULL, NULL) < 0;
    }
    return 0;
}

static void cleanup(Kind k, Call c) {
    if (c == DELETE && k != THREAD_END)
        return;
    switch (k) {
    case SEMA: sceKernelDeleteSema(object); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case MUTEX: sceKernelDeleteMutex(object); break;
    case RWLOCK:
        // The readers that got the lock ended without unlocking it
        sceKernelDeleteRWLock(object);
        break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    case LW_MUTEX: break;
    case THREAD_END: sceKernelDeleteThread(target); break;
    }
}

static void run(Kind k, Call c, const char *name) {
    kind = k;
    run_count = 0;
    for (int i = 0; i < 3; i++) {
        order[i] = 0xF;
        result[i] = 0xDEAD;
    }
    create(k);
    SceUID threads[3];
    for (int i = 0; i < 3; i++) {
        threads[i] = sceKernelCreateThread("waiter", waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }
    const int r = call(k, c);
    // Only now the waiters can run
    TEST_WAIT_FOR(run_count == 3);
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);
    cleanup(k, c);
    PRINTF(r, "%s: the call", name);
    PRINTF(result[0], "%s: the first waiter's result", name);
    PRINTF((order[0] << 8) | (order[1] << 4) | order[2], "%s: run order", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run(SEMA, RELEASE, "semaphore signal of 3");
    run(SEMA, CANCEL, "semaphore cancel");
    run(SEMA, DELETE, "semaphore delete");
    run(FLAG, RELEASE, "event flag set");
    run(FLAG, CANCEL, "event flag cancel");
    run(FLAG, DELETE, "event flag delete");
    run(MUTEX, CANCEL, "mutex cancel");
    run(MUTEX, DELETE, "mutex delete");
    run(RWLOCK, RELEASE, "rwlock write unlock with three readers");
    run(RWLOCK, CANCEL, "rwlock cancel");
    run(RWLOCK, DELETE, "rwlock delete");
    run(EVENT, RELEASE, "manual reset simple event set");
    run(EVENT, CANCEL, "simple event cancel");
    run(EVENT, DELETE, "simple event delete");
    run(PIPE, CANCEL, "message pipe cancel with three receivers");
    run(PIPE, DELETE, "message pipe delete with three receivers");
    run(LW_MUTEX, DELETE, "lightweight mutex delete");
    run(THREAD_END, RELEASE, "the end of a thread three threads wait for");

    test_exit();
    return 0;
}
