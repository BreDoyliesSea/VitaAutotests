// What sceKernelCancelMutex does with each new count, and which sceKernelLockMutexCB calls run a
// callback that is pending on the calling thread.
//
// Cancel: a fresh mutex created with an init count of 1, so the main thread owns it, and a waiter
// thread that waits for it. The main thread cancels it with a new count and a pointer for the
// number of waiters. Each case prints the cancel's result, the number it writes, whether the
// waiter still waits, the waiter's lock result, and the info's current count, whether the main
// thread owns it and the waiters. A waiter that the cancel left waiting gets the mutex when the
// main thread unlocks it, and then reports 0. The cases:
// - a non recursive mutex, new counts -1, -2, -100, the lowest count, 0, 1 and 2
// - a recursive mutex, new count 5
// - a recursive mutex with an init count of 3, new count -4
//
// Lock: the main thread has a callback that counts its runs, and notifies it before each call, so
// it is pending. Each case prints the call's result, the runs of the callback when the call
// returned, the info's current count, and the runs after sceKernelCheckCallback, which shows
// whether the call left it pending. The calls take a timeout of TEST_WAIT_US, which none of them
// is expected to use. Locking a free mutex, a lock that has to wait and an unknown UID with a count
// of 1 are in cb_waits already. The cases here:
// - the owner locks a recursive mutex again
// - the owner locks a non recursive mutex it holds
// - the last cases, which a wrong implementation may run the callback for or may wait on:
//   count 0, -1 and 2 on a free mutex, count 0 and -1 on a free recursive mutex, count 0 on a
//   mutex the caller owns, count 0 on a mutex another thread holds, and count 0 on an unknown UID
//
// On a PS Vita TV on firmware 3.65 every negative count resets the mutex to its init count with the
// main thread as owner, and 0 leaves it free. A count of 2 on a non recursive mutex fails with
// 0x8002000F, writes 0 waiters and leaves the waiter waiting. A cancel that succeeds writes 1
// waiter, the waiter no longer waits by the time the cancel returns, and its lock fails with
// 0x80028007. No lock call that ends without waiting runs the pending callback, whether it succeeds
// as the owner's recursive lock, fails as the owner's lock of a non recursive mutex (0x80028142),
// has a bad count (0x8002000F, checked before the owner) or an unknown UID (0x80028141). The check
// after each runs it.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define NOT_WRITTEN 0x7777
#define BAD_UID 0x12345

static SceUID mutex, held, release, callback;
static volatile int waiter_result, held_ready, calls;

static int counting_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    calls++;
    return 0;
}

// Locks the mutex under test once and unlocks it again if it got it
static int waiter_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    waiter_result = sceKernelLockMutex(mutex, 1, &timeout);
    if (waiter_result == 0)
        sceKernelUnlockMutex(mutex, 1);
    return 0;
}

// Holds the held mutex until the release semaphore is signaled
static int holder_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelLockMutex(held, 1, &timeout);
    held_ready = 1;
    timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(release, 1, &timeout);
    sceKernelUnlockMutex(held, 1);
    return 0;
}

static int read_info(SceUID id, MutexInfo *info) {
    info->size = sizeof(*info);
    return sceKernelGetMutexInfo(id, (SceKernelMutexInfo *)info);
}

static void cancel_case(const char *label, SceUInt attr, int init_count, int new_count) {
    mutex = sceKernelCreateMutex("mutex", attr, init_count, NULL);
    waiter_result = NOT_DONE;
    const SceUID waiter = test_start_thread(waiter_thread);
    TEST_WAIT_FOR(test_in_wait(waiter, WAIT_TYPE_MUTEX, mutex));

    int waiting = NOT_WRITTEN;
    const int result = sceKernelCancelMutex(mutex, new_count, &waiting);
    const int still_waits = test_in_wait(waiter, WAIT_TYPE_MUTEX, mutex);
    MutexInfo info;
    if (result != 0) {
        // The cancel failed, so the waiter is still blocked and the unlock hands the mutex to it
        read_info(mutex, &info);
        sceKernelUnlockMutex(mutex, info.currentCount > 0 ? info.currentCount : 1);
        test_join_thread(waiter);
    } else {
        test_join_thread(waiter);
        read_info(mutex, &info);
    }
    PRINTF(result, "%s: the cancel", label);
    PRINTF(waiting, "%s: the number of waiters it writes", label);
    PRINTF(still_waits, "%s: the waiter still waits", label);
    PRINTF(waiter_result, "%s: the waiter's lock", label);
    PRINTF(info.currentCount, "%s: the count after", label);
    PRINTF(info.currentOwnerId == sceKernelGetThreadId(), "%s: the main thread owns it", label);
    PRINTF(info.numWaitThreads, "%s: the waiters after", label);
    sceKernelDeleteMutex(mutex);
}

// Locks the mutex with callbacks and a callback pending, and prints what happened
static void lock_case(const char *label, SceUID id, int count) {
    SceUInt timeout = TEST_WAIT_US;
    calls = 0;
    sceKernelNotifyCallback(callback, 1);
    const int result = sceKernelLockMutexCB(id, count, &timeout);
    const int ran = calls;
    MutexInfo info;
    const int has_info = read_info(id, &info) == 0;
    sceKernelCheckCallback();
    PRINTF(result, "%s: the lock", label);
    PRINTF(ran, "%s: runs of the pending callback", label);
    if (has_info)
        PRINTF(info.currentCount, "%s: the count after", label);
    PRINTF(calls, "%s: runs after a check of the callbacks", label);
    sceKernelCancelCallback(callback);
}

static void lock_case_new(const char *label, SceUInt attr, int init_count, int count) {
    const SceUID id = sceKernelCreateMutex("mutex", attr, init_count, NULL);
    lock_case(label, id, count);
    sceKernelDeleteMutex(id);
}

int main(void) {
    static const struct {
        const char *label;
        SceUInt attr;
        int init_count;
        int new_count;
    } cancels[] = {
        { "cancel to -1", 0, 1, -1 },
        { "cancel to -2", 0, 1, -2 },
        { "cancel to -100", 0, 1, -100 },
        { "cancel to the lowest count", 0, 1, -2147483647 - 1 },
        { "cancel to 0", 0, 1, 0 },
        { "cancel to 1", 0, 1, 1 },
        { "cancel to 2", 0, 1, 2 },
        { "recursive, cancel to 5", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 1, 5 },
        { "recursive with an init count of 3, cancel to -4", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 3, -4 },
    };
    for (unsigned int i = 0; i < sizeof(cancels) / sizeof(cancels[0]); i++)
        cancel_case(cancels[i].label, cancels[i].attr, cancels[i].init_count, cancels[i].new_count);

    callback = sceKernelCreateCallback("pending", 0, counting_callback, NULL);
    lock_case_new("recursive mutex, the owner locks again", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 1, 1);
    lock_case_new("mutex, the owner locks again", 0, 1, 1);

    // Bad counts and an unknown UID go last, since a wrong implementation may wait on them
    lock_case_new("mutex, count 0", 0, 0, 0);
    lock_case_new("mutex, count -1", 0, 0, -1);
    lock_case_new("mutex, count 2", 0, 0, 2);
    lock_case_new("recursive mutex, count 0", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, 0);
    lock_case_new("recursive mutex, count -1", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, -1);
    lock_case_new("mutex the caller owns, count 0", 0, 1, 0);

    held = sceKernelCreateMutex("held", 0, 0, NULL);
    release = sceKernelCreateSema("release", 0, 0, 1, NULL);
    const SceUID holder = test_start_thread(holder_thread);
    TEST_WAIT_FOR(held_ready);
    lock_case("mutex another thread holds, count 0", held, 0);
    sceKernelSignalSema(release, 1);
    test_join_thread(holder);
    sceKernelDeleteSema(release);
    sceKernelDeleteMutex(held);

    lock_case("unknown UID, count 0", BAD_UID, 0);

    sceKernelDeleteCallback(callback);
    test_exit();
    return 0;
}
