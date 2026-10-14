// More releases and who has the object right after them, before any waiter runs: a recursive
// mutex unlocked to a waiter that asked for a count of 2, a condition and a lightweight condition
// whose signaled waiter waits for the mutex the signaler then unlocks, a write unlock with two
// readers waiting, and an auto reset simple event set with two waiters for the same bit. The
// releasing thread tries to take the object right back after each release. Every thread is pinned
// to core 0 and the waiters run below the main thread. Each value is read first and printed after
// the waiters ended.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0

typedef enum Kind { MUTEX_2, COND, LW_COND, READER, EVENT } Kind;

static Kind kind;
static SceUID object, mutex;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int result[2];

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    SceUInt32 bits;
    switch (kind) {
    case MUTEX_2:
        result[i] = sceKernelLockMutex(object, 2, &timeout);
        if (result[i] == 0)
            sceKernelUnlockMutex(object, 2);
        break;
    case COND:
        sceKernelLockMutex(mutex, 1, NULL);
        result[i] = sceKernelWaitCond(object, &timeout);
        sceKernelUnlockMutex(mutex, 1);
        break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        result[i] = sceKernelWaitLwCond(&lw_cond, &timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case READER:
        result[i] = sceKernelLockReadRWLock(object, &timeout);
        if (result[i] == 0)
            sceKernelUnlockReadRWLock(object);
        break;
    case EVENT: result[i] = sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout); break;
    }
    return 0;
}

static SceUID threads[2];

static void start_waiters(Kind k, int count) {
    kind = k;
    for (int i = 0; i < count; i++) {
        result[i] = 0xDEAD;
        threads[i] = sceKernelCreateThread("waiter", waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }
}

static int waits(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return (info.status & SCE_THREAD_WAITING) != 0;
}

static void join(int count) {
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
}

static void mutex_case(void) {
    object = sceKernelCreateMutex("mutex", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 1, NULL);
    start_waiters(MUTEX_2, 1);
    sceKernelUnlockMutex(object, 1);
    const int still = waits(threads[0]);
    const int try_lock = sceKernelTryLockMutex(object, 1);
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(object, (SceKernelMutexInfo *)&info);
    if (try_lock == 0)
        sceKernelUnlockMutex(object, 1);
    join(1);
    PRINT("recursive mutex, a waiter for a count of 2: it still reports its wait", still);
    PRINT("recursive mutex, a waiter for a count of 2: the unlocker's try lock", try_lock);
    PRINT("recursive mutex, a waiter for a count of 2: the waiter owns it", info.currentOwnerId == threads[0]);
    PRINT("recursive mutex, a waiter for a count of 2: the count", info.currentCount);
    PRINT("recursive mutex, a waiter for a count of 2: the waiter's result", result[0]);
    sceKernelDeleteMutex(object);
}

static void cond_case(void) {
    mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
    object = sceKernelCreateCond("cond", 0, mutex, NULL);
    start_waiters(COND, 1);
    sceKernelLockMutex(mutex, 1, NULL);
    sceKernelSignalCond(object);
    const int after_signal = waits(threads[0]);
    sceKernelUnlockMutex(mutex, 1);
    const int after_unlock = waits(threads[0]);
    const int try_lock = sceKernelTryLockMutex(mutex, 1);
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    if (try_lock == 0)
        sceKernelUnlockMutex(mutex, 1);
    join(1);
    PRINT("condition: the waiter still reports a wait after the signal", after_signal);
    PRINT("condition: the waiter still reports a wait after the unlock", after_unlock);
    PRINT("condition: the signaler's try lock after the unlock", try_lock);
    PRINT("condition: the waiter owns the mutex", info.currentOwnerId == threads[0]);
    PRINT("condition: the waiter's result", result[0]);
    sceKernelDeleteCond(object);
    sceKernelDeleteMutex(mutex);
}

static void lw_cond_case(void) {
    sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
    sceKernelCreateLwCond(&lw_cond, "lwcond", 0, &lw_mutex, NULL);
    start_waiters(LW_COND, 1);
    sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    sceKernelSignalLwCond(&lw_cond);
    const int after_signal = waits(threads[0]);
    sceKernelUnlockLwMutex(&lw_mutex, 1);
    const int after_unlock = waits(threads[0]);
    const int try_lock = sceKernelTryLockLwMutex(&lw_mutex, 1);
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    if (try_lock == 0)
        sceKernelUnlockLwMutex(&lw_mutex, 1);
    join(1);
    PRINT("lightweight condition: the waiter still reports a wait after the signal", after_signal);
    PRINT("lightweight condition: the waiter still reports a wait after the unlock", after_unlock);
    PRINT("lightweight condition: the signaler's try lock after the unlock", try_lock);
    PRINT("lightweight condition: the waiter owns the mutex", info.currentOwnerId == threads[0]);
    PRINT("lightweight condition: the waiter's result", result[0]);
    sceKernelDeleteLwCond(&lw_cond);
    sceKernelDeleteLwMutex(&lw_mutex);
}

static void reader_case(void) {
    object = sceKernelCreateRWLock("rw", 0, NULL);
    sceKernelLockWriteRWLock(object, NULL);
    start_waiters(READER, 2);
    sceKernelUnlockWriteRWLock(object);
    const int first = waits(threads[0]), second = waits(threads[1]);
    const int try_write = sceKernelTryLockWriteRWLock(object);
    if (try_write == 0)
        sceKernelUnlockWriteRWLock(object);
    const int try_read = sceKernelTryLockReadRWLock(object);
    if (try_read == 0)
        sceKernelUnlockReadRWLock(object);
    join(2);
    PRINT("write unlock with two readers waiting: the first still reports its wait", first);
    PRINT("write unlock with two readers waiting: the second still reports its wait", second);
    PRINT("write unlock with two readers waiting: the unlocker's try write lock", try_write);
    PRINT("write unlock with two readers waiting: the unlocker's try read lock", try_read);
    PRINT("write unlock with two readers waiting: the first reader's result", result[0]);
    PRINT("write unlock with two readers waiting: the second reader's result", result[1]);
    sceKernelDeleteRWLock(object);
}

static void event_case(void) {
    object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL);
    start_waiters(EVENT, 2);
    sceKernelSetEvent(object, EVENT_BIT_A, 0);
    const int first = waits(threads[0]), second = waits(threads[1]);
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(object, &pattern);
    // Ends the second wait if the set only reached the first
    TEST_WAIT_FOR(result[0] != (int)0xDEAD);
    if (second)
        sceKernelSetEvent(object, EVENT_BIT_A, 0);
    join(2);
    PRINT("auto reset event set with two waiters: the first still reports its wait", first);
    PRINT("auto reset event set with two waiters: the second still reports its wait", second);
    PRINT("auto reset event set with two waiters: the bit after the set", pattern & EVENT_BIT_A);
    PRINT("auto reset event set with two waiters: the first's result", result[0]);
    PRINT("auto reset event set with two waiters: the second's result", result[1]);
    sceKernelDeleteSimpleEvent(object);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    mutex_case();
    cond_case();
    lw_cond_case();
    reader_case();
    event_case();

    test_exit();
    return 0;
}
