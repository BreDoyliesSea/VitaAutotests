// Whether a release gives the object to a waiting thread before that thread runs, or leaves it for
// whoever takes it first: after each release, the releasing thread tries to take the object right
// back. Covered: a semaphore, an event flag with a clearing and a non clearing waiter, a
// lightweight mutex, a rwlock write unlock to a writer and a read unlock to a writer, an auto reset
// and a manual reset simple event, and a message pipe send to a waiting receiver. The heavy mutex
// is in queue_order. Every thread is pinned to core 0 and the waiters run below the main thread, so
// a waiter can't run between the release and the try. Each value is read first and printed after
// the waiter ended, since printing lets the waiter run.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0

typedef enum Kind { SEMA, FLAG_CLEAR, FLAG_KEEP, LW_MUTEX, RW_WRITE, EVENT, PIPE } Kind;

static Kind kind;
static SceUID object;
static SceKernelLwMutexWork lw_mutex;
static volatile int result;
static SceUInt32 bits;
static char pipe_buf[4];

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    switch (kind) {
    case SEMA: result = sceKernelWaitSema(object, 1, &timeout); break;
    case FLAG_CLEAR: result = sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &bits, &timeout); break;
    case FLAG_KEEP: result = sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case LW_MUTEX:
        result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        if (result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case RW_WRITE:
        result = sceKernelLockWriteRWLock(object, &timeout);
        if (result == 0)
            sceKernelUnlockWriteRWLock(object);
        break;
    case EVENT: result = sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout); break;
    case PIPE: result = sceKernelReceiveMsgPipe(object, pipe_buf, sizeof(pipe_buf), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    }
    return 0;
}

static SceUID start_waiter(Kind k) {
    kind = k;
    result = 0xDEAD;
    bits = 0xDEAD;
    const SceUID thread = sceKernelCreateThread("waiter", waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    test_wait_until_waiting(thread);
    return thread;
}

// Whether thread still reports a wait
static int still_waits(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return (info.status & SCE_THREAD_WAITING) != 0;
}

static void sema_case(void) {
    object = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID thread = start_waiter(SEMA);
    sceKernelSignalSema(object, 1);
    const int waits = still_waits(thread);
    const int poll = sceKernelPollSema(object, 1);
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(object, &info);
    // If the poll took the count, the waiter needs another one
    if (poll == 0)
        sceKernelSignalSema(object, 1);
    test_join_thread(thread);
    PRINT("semaphore: the waiter still reports its wait after the signal", waits);
    PRINT("semaphore: the signaler's poll right after it", poll);
    PRINT("semaphore: the count after the poll", info.currentCount);
    PRINT("semaphore: the waiters after the poll", info.numWaitThreads);
    PRINT("semaphore: the waiter's result", result);
    sceKernelDeleteSema(object);
}

static void flag_case(Kind k, const char *name) {
    object = sceKernelCreateEventFlag("flag", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    const SceUID thread = start_waiter(k);
    sceKernelSetEventFlag(object, 0x1);
    const int waits = still_waits(thread);
    SceUInt32 polled = 0xDEAD;
    const int poll = sceKernelPollEventFlag(object, 0x1, SCE_EVENT_WAITOR, &polled);
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(object, &info);
    if (poll != 0)
        sceKernelSetEventFlag(object, 0x1);
    test_join_thread(thread);
    PRINTF(waits, "%s: the waiter still reports its wait after the set", name);
    PRINTF(poll, "%s: the setter's poll right after it", name);
    PRINTF(poll == 0 ? polled : 0, "%s: the bits the poll got", name);
    PRINTF(info.currentPattern, "%s: the bits after the poll", name);
    PRINTF(info.numWaitThreads, "%s: the waiters after the poll", name);
    PRINTF(result, "%s: the waiter's result", name);
    PRINTF(bits, "%s: the bits the waiter got", name);
    sceKernelDeleteEventFlag(object);
}

static void lw_mutex_case(void) {
    sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 1, NULL);
    const SceUID thread = start_waiter(LW_MUTEX);
    sceKernelUnlockLwMutex(&lw_mutex, 1);
    const int waits = still_waits(thread);
    const int try_lock = sceKernelTryLockLwMutex(&lw_mutex, 1);
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    if (try_lock == 0)
        sceKernelUnlockLwMutex(&lw_mutex, 1);
    test_join_thread(thread);
    PRINT("lightweight mutex: the waiter still reports its wait after the unlock", waits);
    PRINT("lightweight mutex: the unlocker's try lock right after it", try_lock);
    PRINT("lightweight mutex: the owner after the try is the waiter", info.currentOwnerId == thread);
    PRINT("lightweight mutex: the count after the try", info.currentCount);
    PRINT("lightweight mutex: the waiters after the try", info.numWaitThreads);
    PRINT("lightweight mutex: the waiter's result", result);
    sceKernelDeleteLwMutex(&lw_mutex);
}

static void rwlock_case(int read_held, const char *name) {
    object = sceKernelCreateRWLock("rw", 0, NULL);
    if (read_held)
        sceKernelLockReadRWLock(object, NULL);
    else
        sceKernelLockWriteRWLock(object, NULL);
    const SceUID thread = start_waiter(RW_WRITE);
    if (read_held)
        sceKernelUnlockReadRWLock(object);
    else
        sceKernelUnlockWriteRWLock(object);
    const int waits = still_waits(thread);
    const int try_read = sceKernelTryLockReadRWLock(object);
    if (try_read == 0)
        sceKernelUnlockReadRWLock(object);
    const int try_write = sceKernelTryLockWriteRWLock(object);
    if (try_write == 0)
        sceKernelUnlockWriteRWLock(object);
    test_join_thread(thread);
    PRINTF(waits, "%s: the waiting writer still reports its wait after the unlock", name);
    PRINTF(try_read, "%s: the unlocker's try read lock right after it", name);
    PRINTF(try_write, "%s: the unlocker's try write lock after that", name);
    PRINTF(result, "%s: the writer's result", name);
    sceKernelDeleteRWLock(object);
}

static void event_case(SceUInt32 attr, const char *name) {
    object = sceKernelCreateSimpleEvent("event", attr, 0, NULL);
    const SceUID thread = start_waiter(EVENT);
    sceKernelSetEvent(object, EVENT_BIT_A, 0);
    const int waits = still_waits(thread);
    SceUInt32 polled = 0xDEAD;
    const int poll = sceKernelPollEvent(object, EVENT_BIT_A, &polled, NULL);
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(object, &pattern);
    if (poll != 0)
        sceKernelSetEvent(object, EVENT_BIT_A, 0);
    test_join_thread(thread);
    PRINTF(waits, "%s: the waiter still reports its wait after the set", name);
    PRINTF(poll, "%s: the setter's poll right after it", name);
    PRINTF(poll == 0 ? polled : 0, "%s: the bits the poll got", name);
    PRINTF(pattern & EVENT_BIT_A, "%s: the bit after the poll", name);
    PRINTF(result, "%s: the waiter's result", name);
    PRINTF(bits, "%s: the bits the waiter got", name);
    sceKernelDeleteSimpleEvent(object);
}

static void pipe_case(void) {
    object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL);
    sceClibMemset(pipe_buf, 0, sizeof(pipe_buf));
    const SceUID thread = start_waiter(PIPE);
    SceSize size = 0;
    const int send = sceKernelTrySendMsgPipe(object, "wxyz", 4, MSG_PIPE_MODE_FULL, &size);
    const int waits = still_waits(thread);
    char got[4] = { 0 };
    SceSize got_size = 0xDEAD;
    const int receive = sceKernelTryReceiveMsgPipe(object, got, sizeof(got), MSG_PIPE_MODE_ASAP, &got_size);
    if (receive == 0)
        sceKernelTrySendMsgPipe(object, "WXYZ", 4, MSG_PIPE_MODE_FULL, &size);
    test_join_thread(thread);
    PRINT("message pipe: the send to a waiting receiver", send);
    PRINT("message pipe: the receiver still reports its wait after the send", waits);
    PRINT("message pipe: the sender's receive right after it", receive);
    PRINT("message pipe: the bytes that receive got", receive == 0 ? (int)got_size : 0);
    PRINT("message pipe: the receiver's result", result);
    PRINT("message pipe: the receiver got the first send", sceClibMemcmp(pipe_buf, "wxyz", 4) == 0);
    sceKernelDeleteMsgPipe(object);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    sema_case();
    flag_case(FLAG_CLEAR, "event flag, a clearing waiter");
    flag_case(FLAG_KEEP, "event flag, a waiter that doesn't clear");
    lw_mutex_case();
    rwlock_case(0, "rwlock write unlock");
    rwlock_case(1, "rwlock read unlock");
    event_case(EVENT_ATTR_AUTO_RESET, "auto reset simple event");
    event_case(EVENT_ATTR_MANUAL_RESET, "manual reset simple event");
    pipe_case();

    test_exit();
    return 0;
}
