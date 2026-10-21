// The uid field of the info that sceKernelGetLwCondInfoById and sceKernelGetLwMutexInfoById fill,
// compared with the uid field that sceKernelGetLwCondInfo and sceKernelGetLwMutexInfo fill from the
// work area, and with the UID the by UID call was given. The by UID call is always given the uid
// that the by work area call reported. The output only has comparisons, so it doesn't depend on UID
// values.
//
// For a lightweight condition variable and a lightweight mutex, each made fresh, once with nobody
// waiting and once with a thread waiting on it (a condition waiter, a mutex waiter, read while it
// waits), a case prints:
// - whether the uid from the work area call is the UID that the work area holds
// - the result of the by UID call
// - how the uid it reports compares with the UID it was given and with the UID in the work area:
//   equal, 0, negative, same object class
// - whether that reported uid works as a UID in another by UID call
// - whether each other field of the two infos is the same: size, name, attribute, work area
//   pointer, mutex (pointer or owner) and counts, and the number of waiters
// A heavy condition variable's uid field from sceKernelGetCondInfo is printed the same way, idle
// and with a waiter, as a reference.
//
// On a PS Vita TV on firmware 3.65 the lightweight condition variable's info by UID reports a uid
// that is neither the UID it was given nor the one in the work area, and is of another class, and
// an info by UID call with it fails with 0x800281C1. Every other field is the same as by work area.
// The lightweight mutex and the heavy condition variable report the UID they were given.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD

enum Kind { LW_COND, LW_MUTEX, HEAVY_COND };

static SceKernelLwMutexWork lw_mutex, lw_cond_mutex;
static SceKernelLwCondWork lw_cond;
static SceUID heavy_mutex, heavy_cond;
static volatile int wait_result;

// Waits in the way given by the argument and records the result
static int waiter(SceSize args, void *argp) {
    (void)args;
    SceUInt timeout = TEST_WAIT_US;
    switch (*(const int *)argp) {
    case LW_COND:
        sceKernelLockLwMutex(&lw_cond_mutex, 1, NULL);
        wait_result = sceKernelWaitLwCond(&lw_cond, &timeout);
        sceKernelUnlockLwMutex(&lw_cond_mutex, 1);
        break;
    case LW_MUTEX:
        wait_result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        if (wait_result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case HEAVY_COND:
        sceKernelLockMutex(heavy_mutex, 1, NULL);
        wait_result = sceKernelWaitCond(heavy_cond, &timeout);
        sceKernelUnlockMutex(heavy_mutex, 1);
        break;
    }
    return 0;
}

static int waits_in(SceUID thread, SceUInt32 type) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType == type;
}

// Prints how the uid an info reports compares with the UID the call was given
static void print_uid(const char *when, SceUID reported, SceUID given) {
    PRINTF(reported == given, "%s: the uid is the UID it was given", when);
    PRINTF(reported == 0, "%s: the uid is 0", when);
    PRINTF(reported < 0, "%s: the uid is negative", when);
    PRINTF(get_uid_class(reported) == get_uid_class(given), "%s: the uid has the class of the UID it was given", when);
}

#define SAME(when, what, a, b, field) PRINTF((a).field == (b).field, "%s: the %s is the same by UID", when, what)
#define SAME_NAME(when, a, b) PRINTF(sceClibStrncmp((a).name, (b).name, sizeof((a).name)) == 0, "%s: the name is the same by UID", when)

static void check_lw_cond(const char *when) {
    LwCondInfo by_work = { .size = sizeof(by_work) };
    LwCondInfo by_id = { .size = sizeof(by_id) };
    LwCondInfo again = { .size = sizeof(again) };
    PRINTF(sceKernelGetLwCondInfo(&lw_cond, &by_work), "%s: info by work area", when);
    // The first word of the work area is the condition's UID
    const SceUID in_work = ((const SceUID *)&lw_cond)[0];
    PRINTF(by_work.uid == in_work, "%s: the by work area uid is the UID in the work area", when);
    PRINTF(sceKernelGetLwCondInfoById(by_work.uid, &by_id), "%s: info by UID with the by work area uid", when);
    print_uid(when, by_id.uid, by_work.uid);
    PRINTF(by_id.uid == in_work, "%s: the uid is the UID in the work area", when);
    PRINTF(sceKernelGetLwCondInfoById(by_id.uid, &again), "%s: info by UID with the uid it reported", when);
    PRINTF(again.uid == by_id.uid, "%s: that call reports the same uid", when);
    SAME(when, "size", by_work, by_id, size);
    SAME_NAME(when, by_work, by_id);
    SAME(when, "attribute", by_work, by_id, attr);
    SAME(when, "work area pointer", by_work, by_id, work);
    SAME(when, "mutex pointer", by_work, by_id, lwMutex);
    SAME(when, "waiter count", by_work, by_id, numWaitThreads);
    PRINTF(by_work.numWaitThreads, "%s: the waiters by work area", when);
    PRINTF(by_id.numWaitThreads, "%s: the waiters by UID", when);
}

static void check_lw_mutex(const char *when) {
    LwMutexInfo by_work = { .size = sizeof(by_work) };
    LwMutexInfo by_id = { .size = sizeof(by_id) };
    LwMutexInfo again = { .size = sizeof(again) };
    PRINTF(sceKernelGetLwMutexInfo(&lw_mutex, &by_work), "%s: info by work area", when);
    // The fifth word of the work area is the mutex's UID
    const SceUID in_work = ((const SceUID *)&lw_mutex)[4];
    PRINTF(by_work.uid == in_work, "%s: the by work area uid is the UID in the work area", when);
    PRINTF(sceKernelGetLwMutexInfoById(by_work.uid, &by_id), "%s: info by UID with the by work area uid", when);
    print_uid(when, by_id.uid, by_work.uid);
    PRINTF(by_id.uid == in_work, "%s: the uid is the UID in the work area", when);
    PRINTF(sceKernelGetLwMutexInfoById(by_id.uid, &again), "%s: info by UID with the uid it reported", when);
    PRINTF(again.uid == by_id.uid, "%s: that call reports the same uid", when);
    SAME(when, "size", by_work, by_id, size);
    SAME_NAME(when, by_work, by_id);
    SAME(when, "attribute", by_work, by_id, attr);
    SAME(when, "work area pointer", by_work, by_id, work);
    SAME(when, "initial count", by_work, by_id, initCount);
    SAME(when, "count", by_work, by_id, currentCount);
    SAME(when, "owner", by_work, by_id, currentOwnerId);
    SAME(when, "waiter count", by_work, by_id, numWaitThreads);
    PRINTF(by_work.numWaitThreads, "%s: the waiters by work area", when);
    PRINTF(by_id.numWaitThreads, "%s: the waiters by UID", when);
}

static void check_heavy_cond(const char *when) {
    SceKernelCondInfo info = { .size = sizeof(info) };
    PRINTF(sceKernelGetCondInfo(heavy_cond, &info), "%s: info by UID", when);
    print_uid(when, info.condId, heavy_cond);
    PRINTF(info.numWaitThreads, "%s: the waiters", when);
}

static SceUID start_waiter(int kind) {
    wait_result = NOT_DONE;
    return test_start_thread_arg(waiter, kind);
}

static void run_lw_cond(int with_waiter) {
    const char *when = with_waiter ? "lightweight condition variable with a waiter" : "lightweight condition variable, no waiter";
    sceKernelCreateLwMutex(&lw_cond_mutex, "lw cond mutex", 0, 0, NULL);
    sceKernelCreateLwCond(&lw_cond, "lw cond", 0, &lw_cond_mutex, NULL);
    SceUID thread = -1;
    if (with_waiter) {
        thread = start_waiter(LW_COND);
        TEST_WAIT_FOR(waits_in(thread, WAIT_TYPE_LW_COND));
        PRINTF(waits_in(thread, WAIT_TYPE_LW_COND), "%s: the waiter waits on the condition", when);
    }
    check_lw_cond(when);
    if (with_waiter) {
        sceKernelSignalLwCondAll(&lw_cond);
        test_join_thread(thread);
        PRINTF(wait_result, "%s: the waiter's wait", when);
    }
    sceKernelDeleteLwCond(&lw_cond);
    sceKernelDeleteLwMutex(&lw_cond_mutex);
}

static void run_lw_mutex(int with_waiter) {
    const char *when = with_waiter ? "lightweight mutex with a waiter" : "lightweight mutex, no waiter";
    sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
    SceUID thread = -1;
    if (with_waiter) {
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        thread = start_waiter(LW_MUTEX);
        TEST_WAIT_FOR(waits_in(thread, WAIT_TYPE_LW_MUTEX));
        PRINTF(waits_in(thread, WAIT_TYPE_LW_MUTEX), "%s: the waiter waits on the mutex", when);
    }
    check_lw_mutex(when);
    if (with_waiter) {
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        test_join_thread(thread);
        PRINTF(wait_result, "%s: the waiter's wait", when);
    }
    sceKernelDeleteLwMutex(&lw_mutex);
}

static void run_heavy_cond(int with_waiter) {
    const char *when = with_waiter ? "heavy condition variable with a waiter" : "heavy condition variable, no waiter";
    heavy_mutex = sceKernelCreateMutex("heavy mutex", 0, 0, NULL);
    heavy_cond = sceKernelCreateCond("heavy cond", 0, heavy_mutex, NULL);
    SceUID thread = -1;
    if (with_waiter) {
        thread = start_waiter(HEAVY_COND);
        TEST_WAIT_FOR(waits_in(thread, WAIT_TYPE_COND));
        PRINTF(waits_in(thread, WAIT_TYPE_COND), "%s: the waiter waits on the condition", when);
    }
    check_heavy_cond(when);
    if (with_waiter) {
        sceKernelSignalCondAll(heavy_cond);
        test_join_thread(thread);
        PRINTF(wait_result, "%s: the waiter's wait", when);
    }
    sceKernelDeleteCond(heavy_cond);
    sceKernelDeleteMutex(heavy_mutex);
}

int main(void) {
    run_lw_cond(0);
    run_lw_cond(1);
    run_lw_mutex(0);
    run_lw_mutex(1);
    run_heavy_cond(0);
    run_heavy_cond(1);

    test_exit();
    return 0;
}
