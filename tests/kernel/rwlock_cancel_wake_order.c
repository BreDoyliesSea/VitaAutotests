// The order sceKernelCancelRWLock wakes the waiters of a reader/writer lock in. Readers and writers
// wait in a mixed order behind a write lock the main thread holds, in a FIFO lock and in a priority
// lock, with the waiters at one priority so the queue order is their arrival order. The orders are
// reader writer reader writer and writer reader writer reader. The main thread cancels with flag 0.
// Every thread is pinned to core 0, and the waiters run below the main thread, so once the main
// thread blocks, woken waiters of one priority run in the order they were woken. Each waiter
// appends its number (its place in the queue, from 1) to a record when its wait returns. A case
// prints the waiting counts before the cancel, the counts the cancel wrote, the order as the hex
// digits of one value (0x1234 is the queue order), each result, and the lock after the cancel.
//
// On a PS Vita TV on firmware 3.65 the cancel wakes the waiters in queue order, readers and writers
// alike, so they run as 0x1234 in every case. It counts 2 readers and 2 writers, and each wait
// fails with 0x80028007. The cancel also drops the main thread's write lock, so the lock has a
// count of 0 and no owner after it, and the main thread's write unlock fails with 0x800281E4.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0
#define WAITERS 4
#define NOT_DONE 0xDEAD

static SceUID rwlock;
static const char *current_kinds;
static int woken;
static int order[WAITERS];
static volatile int lock_result[WAITERS];

static int in_rwlock_wait(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && (info.status & SCE_THREAD_WAITING) &&
           (info.waitType & ~WAIT_TYPE_CB) == WAIT_TYPE_RW_LOCK;
}

static SceKernelRWLockInfo rwlock_info(void) {
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    sceKernelGetRWLockInfo(rwlock, &info);
    return info;
}

// Waits for the lock as a reader or a writer, as current_kinds says, then records its place
static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    lock_result[i] = current_kinds[i] == 'W' ? sceKernelLockWriteRWLock(rwlock, &timeout)
                                             : sceKernelLockReadRWLock(rwlock, &timeout);
    order[__atomic_fetch_add(&woken, 1, __ATOMIC_SEQ_CST)] = i + 1;
    return 0;
}

static SceUID start(int arg) {
    const SceUID thread = sceKernelCreateThread(
        "waiter", waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(arg), &arg);
    return thread;
}

static void wake_order_case(const char *name, SceUInt32 attr, const char *kinds) {
    SceUID threads[WAITERS];
    current_kinds = kinds;
    __atomic_store_n(&woken, 0, __ATOMIC_SEQ_CST);
    rwlock = sceKernelCreateRWLock("rwlock", attr, NULL);
    sceKernelLockWriteRWLock(rwlock, NULL);
    for (int i = 0; i < WAITERS; i++) {
        order[i] = 0;
        lock_result[i] = NOT_DONE;
        threads[i] = start(i);
        TEST_WAIT_FOR(in_rwlock_wait(threads[i]));
    }
    SceKernelRWLockInfo info = rwlock_info();
    PRINTF(info.numReadWaitThreads, "%s: waiting readers", name);
    PRINTF(info.numWriteWaitThreads, "%s: waiting writers", name);

    SceUInt32 readers = NOT_DONE, writers = NOT_DONE;
    PRINTF(sceKernelCancelRWLock(rwlock, &readers, &writers, 0), "%s: cancel", name);
    TEST_WAIT_FOR(__atomic_load_n(&woken, __ATOMIC_SEQ_CST) == WAITERS);
    for (int i = 0; i < WAITERS; i++)
        test_join_thread(threads[i]);
    PRINTF(readers, "%s: the readers the cancel counted", name);
    PRINTF(writers, "%s: the writers the cancel counted", name);
    PRINTF(order[0] << 12 | order[1] << 8 | order[2] << 4 | order[3], "%s: the waiters in the order they ran", name);
    for (int i = 0; i < WAITERS; i++)
        PRINTF(lock_result[i], "%s: waiter %d, a %s, result", name, i + 1, kinds[i] == 'W' ? "writer" : "reader");
    info = rwlock_info();
    PRINTF(info.lockCount, "%s: the lock count after", name);
    PRINTF(info.writeOwnerId == 0, "%s: the lock has no write owner after", name);
    PRINTF(sceKernelUnlockWriteRWLock(rwlock), "%s: the main thread's write unlock after", name);
    sceKernelDeleteRWLock(rwlock);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    wake_order_case("FIFO, reader writer reader writer", 0, "RWRW");
    wake_order_case("FIFO, writer reader writer reader", 0, "WRWR");
    wake_order_case("priority queue, reader writer reader writer", SCE_KERNEL_ATTR_THREAD_PRIO, "RWRW");
    wake_order_case("priority queue, writer reader writer reader", SCE_KERNEL_ATTR_THREAD_PRIO, "WRWR");

    test_exit();
    return 0;
}
