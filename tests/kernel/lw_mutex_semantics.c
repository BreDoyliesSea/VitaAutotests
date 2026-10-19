// Lightweight mutex waits: the order waiters get the mutex in, FIFO and with the priority
// attribute, a waiter that asks for a count above 1, an unlock that leaves part of a recursive
// count with a waiter, a callback that runs during a lock that runs callbacks, and whether an
// unlock hands the mutex to the waiter before the unlocking thread can take it back.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define LOW_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER + 2)
#define MID_PRIORITY SCE_KERNEL_DEFAULT_PRIORITY_USER
#define HIGH_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER - 2)

static SceKernelLwMutexWork mutex;
static volatile SceUID callback;
static volatile int lock_result[3], unlock_result[3], count_held[3], order[3], woken, done[3], want[3];
static volatile int callback_runs, callback_arg, release;

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

static LwMutexInfo mutex_info(void) {
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelGetLwMutexInfo(&mutex, &info);
    return info;
}

// Locks the mutex with a count of want[i], notes its turn and the count it holds, and unlocks it
static int locker(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    lock_result[i] = sceKernelLockLwMutex(&mutex, want[i], &timeout);
    if (lock_result[i] == 0) {
        order[i] = ++woken;
        count_held[i] = mutex_info().currentCount;
        unlock_result[i] = sceKernelUnlockLwMutex(&mutex, want[i]);
    }
    done[i] = 1;
    return 0;
}

// Locks the mutex with a count of want[i] and holds it until the test sets release
static int holder(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    lock_result[i] = sceKernelLockLwMutex(&mutex, want[i], &timeout);
    if (lock_result[i] == 0) {
        TEST_WAIT_FOR(release);
        unlock_result[i] = sceKernelUnlockLwMutex(&mutex, want[i]);
    }
    done[i] = 1;
    return 0;
}

static int recording_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)common;
    callback_runs++;
    callback_arg = arg;
    return 0;
}

// Locks the mutex with callbacks, with a callback of its own that the test notifies
static int callback_locker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("callback", 0, recording_callback, NULL);
    SceUInt32 timeout = TEST_WAIT_US;
    lock_result[0] = sceKernelLockLwMutexCB(&mutex, 1, &timeout);
    if (lock_result[0] == 0)
        unlock_result[0] = sceKernelUnlockLwMutex(&mutex, 1);
    done[0] = 1;
    sceKernelDeleteCallback(callback);
    return 0;
}

static SceUID start_locker(SceKernelThreadEntry entry, int i, int count, int priority, int affinity) {
    lock_result[i] = unlock_result[i] = count_held[i] = 0xDEAD;
    order[i] = done[i] = 0;
    want[i] = count;
    const SceUID thread = sceKernelCreateThread("locker", entry, priority, 0x4000, 0, affinity, NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    test_wait_until_waiting(thread);
    return thread;
}

static void create(SceUInt32 attr, int count) {
    sceKernelCreateLwMutex(&mutex, "mutex", attr, count, NULL);
    woken = 0;
}

int main(void) {
    SceUID threads[3];

    // The order three waiters get the mutex in, queued low, high, then middle priority
    static const SceUInt32 attrs[] = { 0, SCE_KERNEL_ATTR_THREAD_PRIO };
    static const char *const names[] = { "FIFO", "priority" };
    for (int a = 0; a < 2; a++) {
        create(attrs[a], 1);
        threads[0] = start_locker(locker, 0, 1, LOW_PRIORITY, 0);
        threads[1] = start_locker(locker, 1, 1, HIGH_PRIORITY, 0);
        threads[2] = start_locker(locker, 2, 1, MID_PRIORITY, 0);
        PRINTF(mutex_info().numWaitThreads, "%s: the waiters", names[a]);
        sceKernelUnlockLwMutex(&mutex, 1);
        for (int i = 0; i < 3; i++)
            test_join_thread(threads[i]);
        PRINTF(order[0], "%s: the low priority waiter, queued first, gets it as", names[a]);
        PRINTF(order[1], "%s: the high priority waiter, queued second, gets it as", names[a]);
        PRINTF(order[2], "%s: the middle priority waiter, queued third, gets it as", names[a]);
        sceKernelDeleteLwMutex(&mutex);
    }

    // A waiter that asks for a count of 3 on a recursive mutex
    create(SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 1);
    threads[0] = start_locker(locker, 0, 3, MID_PRIORITY, 0);
    sceKernelUnlockLwMutex(&mutex, 1);
    test_join_thread(threads[0]);
    PRINT("a waiter for a count of 3: its lock", lock_result[0]);
    PRINT("a waiter for a count of 3: the count it holds", count_held[0]);
    PRINT("a waiter for a count of 3: its unlock of 3", unlock_result[0]);

    // An unlock that leaves part of the count keeps the waiter waiting
    sceKernelLockLwMutex(&mutex, 2, NULL);
    threads[0] = start_locker(locker, 0, 1, MID_PRIORITY, 0);
    PRINT("held twice with a waiter: unlock once", sceKernelUnlockLwMutex(&mutex, 1));
    PRINT("held twice with a waiter: the waiter still waits", waits_in(threads[0], WAIT_TYPE_LW_MUTEX));
    LwMutexInfo info = mutex_info();
    PRINT("held twice with a waiter: the count after one unlock", info.currentCount);
    PRINT("held twice with a waiter: this thread still owns it", info.currentOwnerId == sceKernelGetThreadId());
    PRINT("held twice with a waiter: the waiters after one unlock", info.numWaitThreads);
    PRINT("held twice with a waiter: unlock again", sceKernelUnlockLwMutex(&mutex, 1));
    test_join_thread(threads[0]);
    PRINT("held twice with a waiter: the waiter's lock", lock_result[0]);
    sceKernelDeleteLwMutex(&mutex);

    // A waiter for a count of 2 on a mutex without the recursive attribute
    create(0, 1);
    lock_result[0] = 0xDEAD;
    want[0] = 2;
    done[0] = 0;
    threads[0] = test_start_thread_arg(locker, 0);
    TEST_WAIT_FOR(done[0] || waits_in(threads[0], WAIT_TYPE_LW_MUTEX));
    PRINT("not recursive: a waiter for a count of 2 returns at once", done[0]);
    sceKernelUnlockLwMutex(&mutex, 1);
    test_join_thread(threads[0]);
    PRINT("not recursive: its lock", lock_result[0]);
    sceKernelDeleteLwMutex(&mutex);

    // A callback notified while its thread waits in a lock that runs callbacks
    create(0, 1);
    callback_runs = 0;
    callback_arg = 0;
    threads[0] = start_locker(callback_locker, 0, 1, MID_PRIORITY, 0);
    PRINT("lock with callbacks: notify the waiter's callback", sceKernelNotifyCallback(callback, 0x1234));
    TEST_WAIT_FOR(callback_runs != 0);
    TEST_WAIT_FOR(done[0] || waits_in(threads[0], WAIT_TYPE_LW_MUTEX));
    PRINT("lock with callbacks: runs of the callback", callback_runs);
    PRINT("lock with callbacks: the callback's argument", callback_arg);
    PRINT("lock with callbacks: the waiter waits again after it", waits_in(threads[0], WAIT_TYPE_LW_MUTEX));
    PRINT("lock with callbacks: the waiter is done before the unlock", done[0]);
    sceKernelUnlockLwMutex(&mutex, 1);
    test_join_thread(threads[0]);
    PRINT("lock with callbacks: its lock", lock_result[0]);
    PRINT("lock with callbacks: its unlock", unlock_result[0]);
    sceKernelDeleteLwMutex(&mutex);

    // An unlock with a waiter, then a try lock right away by the thread that unlocked. The waiter
    // has a lower priority and the same core as this thread, so it can't run in between, and it
    // holds the mutex until the test is done looking, for an emulator that runs it anyway.
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    for (int a = 0; a < 2; a++) {
        create(attrs[a], 1);
        release = 0;
        threads[0] = start_locker(holder, 0, 1, LOW_PRIORITY, SCE_KERNEL_CPU_MASK_USER_0);
        sceKernelUnlockLwMutex(&mutex, 1);
        info = mutex_info();
        const int try_result = sceKernelTryLockLwMutex(&mutex, 1);
        PRINTF(info.currentOwnerId == threads[0], "%s: right after the unlock the waiter owns the mutex", names[a]);
        PRINTF(info.currentCount, "%s: the count right after the unlock", names[a]);
        PRINTF(info.numWaitThreads, "%s: the waiters right after the unlock", names[a]);
        PRINTF(try_result, "%s: try lock right after unlocking to a waiter", names[a]);
        if (try_result == 0)
            sceKernelUnlockLwMutex(&mutex, 1);
        release = 1;
        test_join_thread(threads[0]);
        PRINTF(lock_result[0], "%s: the waiter's lock", names[a]);
        sceKernelDeleteLwMutex(&mutex);
    }

    test_exit();
    return 0;
}
