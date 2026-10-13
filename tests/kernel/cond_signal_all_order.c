// The order waiters of a condition variable get a priority ordered mutex back after a signal to all
// of them, and what a waiter reports while it waits for the mutex. The same for a lightweight
// condition variable. The waiters have different priorities, so the order they reach the mutex
// in doesn't matter, and each one records its turn while it holds the mutex.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define LOW_PRIORITY 0x10000102
#define MID_PRIORITY 0x10000100
#define HIGH_PRIORITY 0x100000FE

static SceUID cond, mutex;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int lightweight, order[3], woken;

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    if (lightweight) {
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        if (sceKernelWaitLwCond(&lw_cond, &timeout) == 0)
            order[i] = ++woken;
        sceKernelUnlockLwMutex(&lw_mutex, 1);
    } else {
        sceKernelLockMutex(mutex, 1, NULL);
        if (sceKernelWaitCond(cond, &timeout) == 0)
            order[i] = ++woken;
        sceKernelUnlockMutex(mutex, 1);
    }
    return 0;
}

// Whether thread waits, in another wait than the one for the signal
static int past_signal(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0 || info.waitType == 0)
        return 0;
    return info.waitType != (lightweight ? 0x200000u : 0x100u);
}

static void run(const char *name, int lw, SceUInt32 cond_attr, SceUInt32 mutex_attr, const int priorities[3]) {
    lightweight = lw;
    woken = order[0] = order[1] = order[2] = 0;
    SceUID cond_uid, mutex_uid;
    if (lw) {
        sceKernelCreateLwMutex(&lw_mutex, "lw mutex", mutex_attr, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lw cond", cond_attr, &lw_mutex, NULL);
        LwMutexInfo mutex_info = { .size = sizeof(mutex_info) };
        sceKernelGetLwMutexInfo(&lw_mutex, &mutex_info);
        LwCondInfo cond_info = { .size = sizeof(cond_info) };
        sceKernelGetLwCondInfo(&lw_cond, &cond_info);
        mutex_uid = mutex_info.uid;
        cond_uid = cond_info.uid;
    } else {
        mutex = mutex_uid = sceKernelCreateMutex("mutex", mutex_attr, 0, NULL);
        cond = cond_uid = sceKernelCreateCond("cond", cond_attr, mutex, NULL);
    }
    SceUID threads[3];
    for (int i = 0; i < 3; i++) {
        threads[i] = sceKernelCreateThread("waiter", waiter, priorities[i], 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }

    // Every waiter moves to the mutex, which this thread holds until all of them wait for it
    if (lw) {
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        sceKernelSignalLwCondAll(&lw_cond);
    } else {
        sceKernelLockMutex(mutex, 1, NULL);
        sceKernelSignalCondAll(cond);
    }
    TEST_WAIT_FOR(past_signal(threads[0]) && past_signal(threads[1]) && past_signal(threads[2]));
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(threads[0], &info);
    if (lw)
        sceKernelUnlockLwMutex(&lw_mutex, 1);
    else
        sceKernelUnlockMutex(mutex, 1);
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);

    PRINTF(info.waitType, "%s: the wait type while waiting for the mutex", name);
    PRINTF(info.waitId == cond_uid, "%s: its wait UID is the condition", name);
    PRINTF(info.waitId == mutex_uid, "%s: its wait UID is the mutex", name);
    for (int i = 0; i < 3; i++)
        PRINTF(order[i], "%s: waiter %d gets the mutex as", name, i + 1);

    if (lw) {
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_mutex);
    } else {
        sceKernelDeleteCond(cond);
        sceKernelDeleteMutex(mutex);
    }
}

int main(void) {
    static const int rising[3] = { LOW_PRIORITY, MID_PRIORITY, HIGH_PRIORITY };
    run("priority condition and mutex", 0, ATTR_TH_PRIO, ATTR_TH_PRIO, rising);
    run("FIFO condition, priority mutex", 0, 0, ATTR_TH_PRIO, rising);
    run("lightweight, priority condition and mutex", 1, ATTR_TH_PRIO, ATTR_TH_PRIO, rising);
    run("lightweight, FIFO condition, priority mutex", 1, 0, ATTR_TH_PRIO, rising);

    test_exit();
    return 0;
}
