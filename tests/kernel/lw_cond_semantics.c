// Lightweight condition variable semantics: who a signal to one thread wakes, the order of a
// priority condition, where signaled waiters go while the lightweight mutex is held, a signal to a
// thread that waits for the mutex or on another condition variable, a wait whose mutex another
// thread holds, the mutex count during and after a wait with a recursive mutex locked twice, a wait
// that hands the mutex to a thread waiting to lock it, and timeouts that pass while the mutex is
// held by another thread, before and after a signal.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define LOW_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER + 2)
#define MID_PRIORITY SCE_KERNEL_DEFAULT_PRIORITY_USER
#define HIGH_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER - 2)

static SceKernelLwMutexWork mutex;
static SceKernelLwCondWork cond, other_cond;
static SceUInt32 wait_timeout[3];
static volatile int wait_result[3], unlock_result[3], extra_unlock[3], done[3], lock_count[3], order[3], woken;
static volatile int locker_result;

// The type of the wait thread is in, without the callback bit, or 0 if it doesn't wait
static SceUInt32 wait_type(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0 || !(info.status & SCE_THREAD_WAITING))
        return 0;
    return info.waitType & ~WAIT_TYPE_CB;
}

static int waits_in(SceUID thread, SceUInt32 type) {
    return wait_type(thread) == type;
}

static int cond_waiters(SceKernelLwCondWork *work) {
    LwCondInfo info = { .size = sizeof(info) };
    sceKernelGetLwCondInfo(work, &info);
    return info.numWaitThreads;
}

static LwMutexInfo mutex_info(void) {
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelGetLwMutexInfo(&mutex, &info);
    return info;
}

// Locks the mutex lock_count[i] times, waits on cond with wait_timeout[i], notes its turn, and
// unlocks once, then the rest
static int cond_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    sceKernelLockLwMutex(&mutex, lock_count[i], NULL);
    SceUInt32 timeout = wait_timeout[i];
    wait_result[i] = sceKernelWaitLwCond(&cond, &timeout);
    wait_timeout[i] = timeout;
    if (wait_result[i] == 0)
        order[i] = ++woken;
    unlock_result[i] = sceKernelUnlockLwMutex(&mutex, 1);
    if (lock_count[i] > 1)
        extra_unlock[i] = sceKernelUnlockLwMutex(&mutex, lock_count[i] - 1);
    done[i] = 1;
    return 0;
}

// The same on other_cond
static int other_cond_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    sceKernelLockLwMutex(&mutex, 1, NULL);
    SceUInt32 timeout = TEST_WAIT_US;
    wait_result[i] = sceKernelWaitLwCond(&other_cond, &timeout);
    unlock_result[i] = sceKernelUnlockLwMutex(&mutex, 1);
    done[i] = 1;
    return 0;
}

// Waits to lock the mutex, and unlocks it again
static int mutex_locker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    locker_result = sceKernelLockLwMutex(&mutex, 1, &timeout);
    if (locker_result == 0)
        sceKernelUnlockLwMutex(&mutex, 1);
    return 0;
}

// Waits to lock the mutex, signals cond while holding it, and unlocks it
static int locking_signaler(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    locker_result = sceKernelLockLwMutex(&mutex, 1, &timeout);
    if (locker_result == 0) {
        sceKernelSignalLwCond(&cond);
        sceKernelUnlockLwMutex(&mutex, 1);
    }
    return 0;
}

// Waits on cond without locking the mutex first
static int unlocked_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 1000;
    wait_result[0] = sceKernelWaitLwCond(&cond, &timeout);
    unlock_result[0] = sceKernelUnlockLwMutex(&mutex, 1);
    done[0] = 1;
    return 0;
}

static SceUID start_waiter(SceKernelThreadEntry entry, int i, int count, SceUInt32 timeout, int priority) {
    wait_result[i] = unlock_result[i] = extra_unlock[i] = 0xDEAD;
    done[i] = order[i] = 0;
    lock_count[i] = count;
    wait_timeout[i] = timeout;
    const SceUID thread = sceKernelCreateThread("waiter", entry, priority, 0x4000, 0, 0, NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    test_wait_until_waiting(thread);
    return thread;
}

static void create(SceUInt32 mutex_attr, SceUInt32 cond_attr) {
    sceKernelCreateLwMutex(&mutex, "mutex", mutex_attr, 0, NULL);
    sceKernelCreateLwCond(&cond, "cond", cond_attr, &mutex, NULL);
    woken = 0;
}

static void destroy(void) {
    sceKernelDeleteLwCond(&cond);
    sceKernelDeleteLwMutex(&mutex);
}

int main(void) {
    SceUID threads[3];

    // A signal to one of two waiters wakes only that one
    create(0, 0);
    threads[0] = start_waiter(cond_waiter, 0, 1, TEST_WAIT_US, MID_PRIORITY);
    threads[1] = start_waiter(cond_waiter, 1, 1, TEST_WAIT_US, MID_PRIORITY);
    PRINT("signal the second of two waiters", sceKernelSignalLwCondTo(&cond, threads[1]));
    TEST_WAIT_FOR(done[1]);
    PRINT("the second waiter's wait", wait_result[1]);
    PRINT("the first one still waits", waits_in(threads[0], WAIT_TYPE_LW_COND));
    sceKernelWaitThreadEnd(threads[1], NULL, NULL);
    PRINT("signal the ended second waiter", sceKernelSignalLwCondTo(&cond, threads[1]));
    PRINT("signal", sceKernelSignalLwCond(&cond));
    test_join_thread(threads[0]);
    test_join_thread(threads[1]);
    PRINT("the first waiter's wait", wait_result[0]);
    destroy();

    // A priority condition wakes the higher priority waiter first, a FIFO one the first waiter
    static const SceUInt32 cond_attrs[] = { 0, SCE_KERNEL_ATTR_THREAD_PRIO };
    static const char *const cond_names[] = { "FIFO", "priority" };
    for (int c = 0; c < 2; c++) {
        create(0, cond_attrs[c]);
        threads[0] = start_waiter(cond_waiter, 0, 1, TEST_WAIT_US, LOW_PRIORITY);
        threads[1] = start_waiter(cond_waiter, 1, 1, TEST_WAIT_US, HIGH_PRIORITY);
        threads[2] = start_waiter(cond_waiter, 2, 1, TEST_WAIT_US, MID_PRIORITY);
        for (int s = 0; s < 3; s++) {
            sceKernelSignalLwCond(&cond);
            TEST_WAIT_FOR(woken == s + 1);
        }
        for (int i = 0; i < 3; i++)
            test_join_thread(threads[i]);
        PRINTF(order[0], "%s condition: the low priority waiter, queued first, wakes as", cond_names[c]);
        PRINTF(order[1], "%s condition: the high priority waiter, queued second, wakes as", cond_names[c]);
        PRINTF(order[2], "%s condition: the middle priority waiter, queued third, wakes as", cond_names[c]);
        destroy();
    }

    // Signaled waiters move to the mutex while this thread holds it
    create(0, 0);
    for (int i = 0; i < 3; i++)
        threads[i] = start_waiter(cond_waiter, i, 1, TEST_WAIT_US, MID_PRIORITY);
    PRINT("three waiters: the condition's waiters", cond_waiters(&cond));
    sceKernelLockLwMutex(&mutex, 1, NULL);
    PRINT("signal one while holding the mutex", sceKernelSignalLwCond(&cond));
    TEST_WAIT_FOR(waits_in(threads[0], WAIT_TYPE_LW_COND_MUTEX));
    PRINT("the signaled waiter waits for the mutex", waits_in(threads[0], WAIT_TYPE_LW_COND_MUTEX));
    PRINT("the condition's waiters after it", cond_waiters(&cond));
    PRINT("the mutex's waiters after it", mutex_info().numWaitThreads);
    PRINT("signal that waiter again", sceKernelSignalLwCondTo(&cond, threads[0]));
    PRINT("signal all", sceKernelSignalLwCondAll(&cond));
    TEST_WAIT_FOR(waits_in(threads[1], WAIT_TYPE_LW_COND_MUTEX) && waits_in(threads[2], WAIT_TYPE_LW_COND_MUTEX));
    PRINT("the condition's waiters after signal all", cond_waiters(&cond));
    PRINT("the mutex's waiters after signal all", mutex_info().numWaitThreads);
    PRINT("signal all with every waiter on the mutex", sceKernelSignalLwCondAll(&cond));
    PRINT("no waiter is done while the mutex is held", done[0] + done[1] + done[2]);
    sceKernelUnlockLwMutex(&mutex, 1);
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);
    PRINT("each waiter's wait", wait_result[0] | wait_result[1] | wait_result[2]);
    PRINT("each waiter owns the mutex after it", unlock_result[0] | unlock_result[1] | unlock_result[2]);
    destroy();

    // A signal to a thread that waits on another condition variable of the same mutex
    create(0, 0);
    sceKernelCreateLwCond(&other_cond, "other", 0, &mutex, NULL);
    threads[0] = start_waiter(other_cond_waiter, 0, 1, TEST_WAIT_US, MID_PRIORITY);
    PRINT("signal to a thread that waits on another condition", sceKernelSignalLwCondTo(&cond, threads[0]));
    PRINT("it still waits on the other condition", waits_in(threads[0], WAIT_TYPE_LW_COND));
    PRINT("signal all of this condition", sceKernelSignalLwCondAll(&cond));
    PRINT("it still waits after that too", waits_in(threads[0], WAIT_TYPE_LW_COND));
    PRINT("signal it on its own condition", sceKernelSignalLwCondTo(&other_cond, threads[0]));
    test_join_thread(threads[0]);
    PRINT("its wait", wait_result[0]);
    sceKernelDeleteLwCond(&other_cond);

    // A signal to a thread that waits to lock the mutex
    sceKernelLockLwMutex(&mutex, 1, NULL);
    locker_result = 0xDEAD;
    SceUID locker = test_start_thread(mutex_locker);
    test_wait_until_waiting(locker);
    PRINT("signal to a thread that waits to lock the mutex", sceKernelSignalLwCondTo(&cond, locker));
    PRINT("it still waits for the mutex", waits_in(locker, WAIT_TYPE_LW_MUTEX));
    sceKernelUnlockLwMutex(&mutex, 1);
    test_join_thread(locker);
    PRINT("its lock", locker_result);

    // A wait while another thread holds the mutex
    sceKernelLockLwMutex(&mutex, 1, NULL);
    done[0] = 0;
    wait_result[0] = unlock_result[0] = 0xDEAD;
    test_join_thread(test_start_thread(unlocked_waiter));
    PRINT("a wait while another thread holds the mutex", wait_result[0]);
    PRINT("that thread's unlock after it", unlock_result[0]);
    PRINT("this thread still holds the mutex", mutex_info().currentOwnerId == sceKernelGetThreadId());
    sceKernelUnlockLwMutex(&mutex, 1);

    // The wait gives the mutex to a thread waiting to lock it, which then signals
    sceKernelLockLwMutex(&mutex, 1, NULL);
    locker_result = 0xDEAD;
    locker = test_start_thread(locking_signaler);
    test_wait_until_waiting(locker);
    SceUInt32 timeout = TEST_WAIT_US;
    PRINT("a wait with a thread waiting to lock the mutex", sceKernelWaitLwCond(&cond, &timeout));
    PRINT("that thread got the mutex during the wait", locker_result);
    PRINT("this thread owns the mutex after the wait", sceKernelUnlockLwMutex(&mutex, 1));
    test_join_thread(locker);
    destroy();

    // A recursive mutex locked twice: its count while the waiter waits, and after
    create(SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0);
    threads[0] = start_waiter(cond_waiter, 0, 2, TEST_WAIT_US, MID_PRIORITY);
    LwMutexInfo info = mutex_info();
    PRINT("recursive mutex locked twice: the count during the wait", info.currentCount);
    PRINT("recursive mutex locked twice: the waiter owns it during the wait", info.currentOwnerId == threads[0]);
    PRINT("recursive mutex locked twice: try lock during the wait", sceKernelTryLockLwMutex(&mutex, 1));
    sceKernelSignalLwCond(&cond);
    test_join_thread(threads[0]);
    PRINT("recursive mutex locked twice: the wait", wait_result[0]);
    PRINT("recursive mutex locked twice: unlock once after it", unlock_result[0]);
    PRINT("recursive mutex locked twice: then unlock once more", extra_unlock[0]);
    info = mutex_info();
    PRINT("recursive mutex locked twice: the count at the end", info.currentCount);
    destroy();

    // The timeout passes while another thread holds the mutex, with no signal
    create(0, 0);
    threads[0] = start_waiter(cond_waiter, 0, 1, 100000, MID_PRIORITY);
    sceKernelLockLwMutex(&mutex, 1, NULL);
    TEST_WAIT_FOR(done[0] || (wait_type(threads[0]) != 0 && !waits_in(threads[0], WAIT_TYPE_LW_COND)));
    PRINT("timeout with the mutex held: the waiter's wait type after it", wait_type(threads[0]));
    PRINT("timeout with the mutex held: the waiter is done before the mutex is free", done[0]);
    sceKernelUnlockLwMutex(&mutex, 1);
    test_join_thread(threads[0]);
    PRINT("timeout with the mutex held: the wait", wait_result[0]);
    PRINT("timeout with the mutex held: the time left is 0", wait_timeout[0] == 0);
    PRINT("timeout with the mutex held: the waiter owns the mutex after it", unlock_result[0]);

    // A signal comes, then the timeout passes while the waiter waits for the mutex. The test holds
    // the mutex for up to 5 s, far longer than the 100 ms timeout.
    threads[0] = start_waiter(cond_waiter, 0, 1, 100000, MID_PRIORITY);
    sceKernelLockLwMutex(&mutex, 1, NULL);
    sceKernelSignalLwCond(&cond);
    TEST_WAIT_FOR(waits_in(threads[0], WAIT_TYPE_LW_COND_MUTEX));
    TEST_WAIT_FOR(!waits_in(threads[0], WAIT_TYPE_LW_COND_MUTEX));
    PRINT("signal then timeout: the waiter still waits for the mutex long after its timeout", waits_in(threads[0], WAIT_TYPE_LW_COND_MUTEX));
    PRINT("signal then timeout: the waiter is done before the mutex is free", done[0]);
    sceKernelUnlockLwMutex(&mutex, 1);
    test_join_thread(threads[0]);
    PRINT("signal then timeout: the wait", wait_result[0]);
    PRINT("signal then timeout: the time left is 0", wait_timeout[0] == 0);
    PRINT("signal then timeout: the waiter owns the mutex after it", unlock_result[0]);
    destroy();

    test_exit();
    return 0;
}
