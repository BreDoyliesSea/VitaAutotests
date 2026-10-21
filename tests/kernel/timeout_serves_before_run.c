// Whether a wait's timeout ends it at its deadline while its thread can't run, and whether the
// waiters behind it are served at that moment. The main thread runs on core 0 at 0x60 and the
// waiters are pinned to core 0 at lower priorities, so a thread whose wait ended stays ready and
// doesn't run until the main thread blocks. After the waiters queue, the main thread spins for a
// second on the process clock, with no call that blocks or delays, while the head waiter's timeout
// of 400 ms passes. Right after the spin it reads the object and the threads' status, and only then
// blocks in a join. The status is 2 for a ready thread and 8 for a waiting one. Every wait the
// test expects to end by a wake gets a timeout of 6 seconds, far past the spin. Each case uses new
// objects. The cases:
// - a FIFO semaphore with a count of 2, where A waits for 3 with the short timeout and B queues
//   behind it for 1. Whether B got its count at A's deadline shows in the count and the waiters
// - the same on a priority semaphore, where A's priority is above B's
// - an event flag without the multiple waiter attribute, where A waits for bit 0x1 with the short
//   timeout. The flag starts with bit 0x2 set. The main thread polls it for that bit once while A
//   waits and once after A's deadline
// - a mutex that the main thread holds, where A and B wait for it with A's timeout the short one.
//   The mutex is unlocked after the spin, and whoever still waits is next in line
// - a rwlock that the main thread holds for reading, where a writer W waits with the short
//   timeout and a reader R queues behind it. Whether R got its read lock at W's deadline shows in
//   the lock count and the waiting counts
// Each case prints what the main thread read right after the spin, then the result of each waiter.
//
// On a PS Vita TV on firmware 3.65 the timeout ends the head waiter's wait at its deadline while
// the main thread holds the core, and the waiters behind it are served at that moment, before
// either thread runs. B has its count, the event flag is free, and R holds its read lock, with
// every woken thread ready (0x2). The mutex stays the main thread's, and B waits until the unlock.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
// Both below the main thread's, A above B
#define A_PRIORITY 0xA0
#define B_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
// The timeout of the head waiter, which passes during the spin
#define SHORT_US 400000
#define SPIN_US 1000000
// The timeout of every wait that a wake or a take is expected to end, far past the spin
#define LONG_US (3 * TEST_WAIT_US)

static SceUID object;
static volatile int result[2];

// Starts entry on core 0 at the given priority, passing it arg
static SceUID start(SceKernelThreadEntry entry, int priority, int arg) {
    const SceUID thread =
        sceKernelCreateThread("t", entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(arg), &arg);
    return thread;
}

// Spins on the process clock, so the main thread keeps core 0 and never blocks
static void spin(void) {
    const SceUInt64 end = sceKernelGetProcessTimeWide() + SPIN_US;
    while (sceKernelGetProcessTimeWide() < end)
        ;
}

// Returns the status sceKernelGetThreadInfo reports for thread
static int status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

// Waits for a semaphore, A (argument 0) for 3 with the short timeout, B for 1 with the long one
static int sema_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = i == 0 ? SHORT_US : LONG_US;
    result[i] = sceKernelWaitSema(object, i == 0 ? 3 : 1, &timeout);
    return 0;
}

static void sema_case(const char *name, SceUInt32 attr) {
    object = sceKernelCreateSema("sema", attr, 2, 10, NULL);
    result[0] = result[1] = NOT_DONE;
    const SceUID a = start(sema_waiter, A_PRIORITY, 0);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_SEMA, object));
    const SceUID b = start(sema_waiter, B_PRIORITY, 1);
    TEST_WAIT_FOR(test_in_wait(b, WAIT_TYPE_SEMA, object));

    spin();
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(object, &info);
    const int a_status = status_of(a);
    const int b_status = status_of(b);
    test_join_thread(a);
    test_join_thread(b);

    PRINTF(info.currentCount, "%s: the count right after A's deadline", name);
    PRINTF(info.numWaitThreads, "%s: the waiters right after A's deadline", name);
    PRINTF(a_status, "%s: A's status right after its deadline", name);
    PRINTF(b_status, "%s: B's status right after A's deadline", name);
    PRINTF(result[0], "%s: A's result", name);
    PRINTF(result[1], "%s: B's result", name);
    sceKernelDeleteSema(object);
}

// Waits for bit 0x1 of the event flag with the short timeout
static int flag_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = SHORT_US;
    SceUInt32 bits;
    result[0] = sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
    return 0;
}

static void flag_case(const char *name) {
    object = sceKernelCreateEventFlag("flag", 0, 0x2, NULL);
    result[0] = NOT_DONE;
    const SceUID a = start(flag_waiter, A_PRIORITY, 0);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_EVENT_FLAG, object));
    SceUInt32 bits = 0;
    const int poll_before = sceKernelPollEventFlag(object, 0x2, SCE_EVENT_WAITOR, &bits);

    spin();
    const int poll_after = sceKernelPollEventFlag(object, 0x2, SCE_EVENT_WAITOR, &bits);
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(object, &info);
    const int a_status = status_of(a);
    test_join_thread(a);

    PRINTF(poll_before, "%s: the main thread's poll while A waits", name);
    PRINTF(poll_after, "%s: the main thread's poll right after A's deadline", name);
    PRINTF(info.numWaitThreads, "%s: the waiters right after A's deadline", name);
    PRINTF(a_status, "%s: A's status right after its deadline", name);
    PRINTF(result[0], "%s: A's result", name);
    sceKernelDeleteEventFlag(object);
}

// Locks the mutex, A (argument 0) with the short timeout, B with the long one, and unlocks it again
// if it got it
static int mutex_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = i == 0 ? SHORT_US : LONG_US;
    result[i] = sceKernelLockMutex(object, 1, &timeout);
    if (result[i] == 0)
        sceKernelUnlockMutex(object, 1);
    return 0;
}

static void mutex_case(const char *name) {
    object = sceKernelCreateMutex("mutex", 0, 0, NULL);
    sceKernelLockMutex(object, 1, NULL);
    result[0] = result[1] = NOT_DONE;
    const SceUID a = start(mutex_waiter, A_PRIORITY, 0);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_MUTEX, object));
    const SceUID b = start(mutex_waiter, B_PRIORITY, 1);
    TEST_WAIT_FOR(test_in_wait(b, WAIT_TYPE_MUTEX, object));

    spin();
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(object, (SceKernelMutexInfo *)&info);
    const int a_status = status_of(a);
    const int b_status = status_of(b);
    const int unlock = sceKernelUnlockMutex(object, 1);
    test_join_thread(a);
    test_join_thread(b);

    PRINTF(info.numWaitThreads, "%s: the waiters right after A's deadline", name);
    PRINTF(info.currentOwnerId == sceKernelGetThreadId(), "%s: the main thread still owns it right after A's deadline", name);
    PRINTF(a_status, "%s: A's status right after its deadline", name);
    PRINTF(b_status, "%s: B's status right after A's deadline", name);
    PRINTF(unlock, "%s: the main thread's unlock", name);
    PRINTF(result[0], "%s: A's result", name);
    PRINTF(result[1], "%s: B's result", name);
    sceKernelDeleteMutex(object);
}

// Locks the rwlock, W (argument 0) for writing with the short timeout, R for reading with the long
// one, and unlocks it again if it got it
static int rwlock_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = i == 0 ? SHORT_US : LONG_US;
    if (i == 0) {
        result[0] = sceKernelLockWriteRWLock(object, &timeout);
        if (result[0] == 0)
            sceKernelUnlockWriteRWLock(object);
    } else {
        result[1] = sceKernelLockReadRWLock(object, &timeout);
        if (result[1] == 0)
            sceKernelUnlockReadRWLock(object);
    }
    return 0;
}

static void rwlock_case(const char *name) {
    object = sceKernelCreateRWLock("rwlock", 0, NULL);
    sceKernelLockReadRWLock(object, NULL);
    result[0] = result[1] = NOT_DONE;
    const SceUID w = start(rwlock_waiter, A_PRIORITY, 0);
    TEST_WAIT_FOR(test_in_wait(w, WAIT_TYPE_RW_LOCK, object));
    const SceUID r = start(rwlock_waiter, B_PRIORITY, 1);
    TEST_WAIT_FOR(test_in_wait(r, WAIT_TYPE_RW_LOCK, object));

    spin();
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    sceKernelGetRWLockInfo(object, &info);
    const int w_status = status_of(w);
    const int r_status = status_of(r);
    test_join_thread(w);
    test_join_thread(r);
    const int unlock = sceKernelUnlockReadRWLock(object);

    PRINTF(info.lockCount, "%s: the lock count right after W's deadline", name);
    PRINTF(info.numReadWaitThreads, "%s: the waiting readers right after W's deadline", name);
    PRINTF(info.numWriteWaitThreads, "%s: the waiting writers right after W's deadline", name);
    PRINTF(w_status, "%s: W's status right after its deadline", name);
    PRINTF(r_status, "%s: R's status right after W's deadline", name);
    PRINTF(result[0], "%s: W's result", name);
    PRINTF(result[1], "%s: R's result", name);
    PRINTF(unlock, "%s: the main thread's read unlock", name);
    sceKernelDeleteRWLock(object);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    sema_case("FIFO semaphore", 0);
    sema_case("priority semaphore", SCE_KERNEL_ATTR_THREAD_PRIO);
    flag_case("event flag");
    mutex_case("mutex");
    rwlock_case("rwlock");

    test_exit();
    return 0;
}
