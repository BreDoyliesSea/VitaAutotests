// What a reader/writer lock reports right after an unlock or a cancel woke waiters, before the
// woken threads run. The main thread is pinned to core 0 at priority 0x60 and the waiters to core 0
// at 0xB0 or 0xA0, so a woken waiter stays ready until the main thread blocks. Right after the
// waking call, and before any call that blocks, the main thread reads the lock's info (lock count,
// write owner, waiting readers and writers), each waiter's thread status, and the results of a try
// write lock and a try read lock of its own, which it unlocks again if they worked. Only then does
// it join the waiters and print what they got. Every case uses a new lock. The cases:
// - a write unlock with two readers waiting
// - a write unlock with a writer and then a reader waiting
// - a write unlock with a reader and then a writer waiting
// - the same in a priority queue, where the reader has the higher priority and waits after the writer
// - a read unlock with a writer waiting
// - the same in a priority queue with two writers, the one that waits second has the higher priority
// - a cancel with a reader, a writer and a reader waiting
//
// On a PS Vita TV on firmware 3.65 the waking call hands the lock over and updates the counts
// before the woken threads run, and they are ready (0x2). A write unlock lets both waiting readers
// in. With a writer waiting, the first writer gets the lock, also past a reader queued before it,
// which keeps waiting, and in a priority queue the higher writer gets it. A cancel leaves the lock
// free with no waiters. The main thread's try write lock fails with 0x800281E6 whenever the lock is
// held, and its try read fails while a writer holds it but succeeds next to the two readers.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define HIGH_PRIORITY 0xA0
#define LOW_PRIORITY 0xB0
#define MAX_WAITERS 3
#define NOT_DONE 0xDEAD

typedef struct Waiter {
    const char *name;
    int write;
    int priority;
} Waiter;

typedef enum Wake { WRITE_UNLOCK, READ_UNLOCK, CANCEL } Wake;

typedef struct Snapshot {
    int queued, wake, info_result;
    SceKernelRWLockInfo info;
    int owner;
    int status[MAX_WAITERS];
    int try_write, try_read;
} Snapshot;

static SceUID rwlock, main_thread;
static SceUID threads[MAX_WAITERS];
static const Waiter *waiters;
static volatile int lock_result[MAX_WAITERS];

// Waits for a read lock or the write lock, depending on the waiter, and unlocks it again
static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    if (waiters[i].write) {
        lock_result[i] = sceKernelLockWriteRWLock(rwlock, &timeout);
        if (lock_result[i] == 0)
            sceKernelUnlockWriteRWLock(rwlock);
    } else {
        lock_result[i] = sceKernelLockReadRWLock(rwlock, &timeout);
        if (lock_result[i] == 0)
            sceKernelUnlockReadRWLock(rwlock);
    }
    return 0;
}

static int waits_for_rwlock(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && (info.status & SCE_THREAD_WAITING) &&
           (info.waitType & ~WAIT_TYPE_CB) == WAIT_TYPE_RW_LOCK;
}

// Reads everything the case reports. Nothing here blocks, and printing does, so it only stores.
static void take_snapshot(Snapshot *s, int count) {
    s->info = (SceKernelRWLockInfo){ .size = sizeof(s->info) };
    s->info_result = sceKernelGetRWLockInfo(rwlock, &s->info);
    // 0 for no write owner, 1 for the main thread, i + 2 for waiter i, 99 for any other thread
    s->owner = s->info.writeOwnerId == 0 ? 0 : s->info.writeOwnerId == main_thread ? 1 : 99;
    for (int i = 0; i < count; i++) {
        if (s->info.writeOwnerId != 0 && s->info.writeOwnerId == threads[i])
            s->owner = i + 2;
        SceKernelThreadInfo info = { .size = sizeof(info) };
        s->status[i] = sceKernelGetThreadInfo(threads[i], &info) < 0 ? -1 : (int)info.status;
    }
    s->try_write = sceKernelTryLockWriteRWLock(rwlock);
    if (s->try_write == 0)
        sceKernelUnlockWriteRWLock(rwlock);
    s->try_read = sceKernelTryLockReadRWLock(rwlock);
    if (s->try_read == 0)
        sceKernelUnlockReadRWLock(rwlock);
}

// Queues the waiters one by one behind a lock the main thread holds, wakes them as the case says,
// and prints what the lock and the waiters showed before they ran and what they got after
static void run(const char *name, SceUInt32 attr, Wake wake, const Waiter *list, int count) {
    Snapshot s = { 0 };
    rwlock = sceKernelCreateRWLock("rwlock", attr, NULL);
    waiters = list;
    if (wake == READ_UNLOCK)
        sceKernelLockReadRWLock(rwlock, NULL);
    else
        sceKernelLockWriteRWLock(rwlock, NULL);
    for (int i = 0; i < count; i++) {
        lock_result[i] = NOT_DONE;
        threads[i] = sceKernelCreateThread(
            "waiter", waiter_entry, list[i].priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        TEST_WAIT_FOR(waits_for_rwlock(threads[i]));
    }
    s.queued = 1;
    for (int i = 0; i < count; i++)
        s.queued &= waits_for_rwlock(threads[i]);

    if (wake == READ_UNLOCK)
        s.wake = sceKernelUnlockReadRWLock(rwlock);
    else if (wake == WRITE_UNLOCK)
        s.wake = sceKernelUnlockWriteRWLock(rwlock);
    else
        s.wake = sceKernelCancelRWLock(rwlock, NULL, NULL, 0);
    take_snapshot(&s, count);

    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);

    PRINTF(s.queued, "%s: all waiters wait before the wake", name);
    PRINTF(s.wake, "%s: the waking call", name);
    if (s.info_result < 0) {
        PRINTF(s.info_result, "%s: info", name);
    } else {
        PRINTF(s.info.lockCount, "%s: lock count after the wake", name);
        PRINTF(s.owner, "%s: write owner after the wake (0 none, 1 main, n + 1 waiter n)", name);
        PRINTF(s.info.numReadWaitThreads, "%s: waiting readers after the wake", name);
        PRINTF(s.info.numWriteWaitThreads, "%s: waiting writers after the wake", name);
    }
    for (int i = 0; i < count; i++)
        PRINTF(s.status[i], "%s: %s's thread status after the wake", name, list[i].name);
    PRINTF(s.try_write, "%s: try write lock after the wake", name);
    PRINTF(s.try_read, "%s: try read lock after the wake", name);
    for (int i = 0; i < count; i++)
        PRINTF(lock_result[i], "%s: %s's lock result", name, list[i].name);
    sceKernelDeleteRWLock(rwlock);
}

int main(void) {
    main_thread = sceKernelGetThreadId();
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    static const Waiter two_readers[] = {
        { "R1", 0, LOW_PRIORITY },
        { "R2", 0, LOW_PRIORITY },
    };
    run("write unlock, two readers", 0, WRITE_UNLOCK, two_readers, 2);

    static const Waiter writer_reader[] = {
        { "W1", 1, LOW_PRIORITY },
        { "R1", 0, LOW_PRIORITY },
    };
    run("write unlock, a writer then a reader", 0, WRITE_UNLOCK, writer_reader, 2);

    static const Waiter reader_writer[] = {
        { "R1", 0, LOW_PRIORITY },
        { "W1", 1, LOW_PRIORITY },
    };
    run("write unlock, a reader then a writer", 0, WRITE_UNLOCK, reader_writer, 2);

    static const Waiter writer_high_reader[] = {
        { "W1", 1, LOW_PRIORITY },
        { "R1", 0, HIGH_PRIORITY },
    };
    run("priority queue, write unlock, a writer then a higher reader", SCE_KERNEL_ATTR_THREAD_PRIO,
        WRITE_UNLOCK, writer_high_reader, 2);

    static const Waiter one_writer[] = {
        { "W1", 1, LOW_PRIORITY },
    };
    run("read unlock, a writer", 0, READ_UNLOCK, one_writer, 1);

    static const Waiter writer_high_writer[] = {
        { "W1", 1, LOW_PRIORITY },
        { "W2", 1, HIGH_PRIORITY },
    };
    run("priority queue, read unlock, a writer then a higher writer", SCE_KERNEL_ATTR_THREAD_PRIO,
        READ_UNLOCK, writer_high_writer, 2);

    static const Waiter mixed[] = {
        { "R1", 0, LOW_PRIORITY },
        { "W1", 1, LOW_PRIORITY },
        { "R2", 0, LOW_PRIORITY },
    };
    run("cancel, a reader, a writer and a reader", 0, CANCEL, mixed, 3);

    test_exit();
    return 0;
}
