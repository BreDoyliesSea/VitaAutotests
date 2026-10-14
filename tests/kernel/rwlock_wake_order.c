// Who gets a reader/writer lock when it is unlocked with readers and writers waiting, in FIFO and in
// priority order: whether the next writer or the waiting readers go first, and whether all waiting
// readers get it together. The waiters queue one at a time in a known order. Each one that gets the
// lock keeps it until the test lets it go, so the test reads who holds the lock in each round
// while nothing can change.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define LOW_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER + 2)
#define MID_PRIORITY SCE_KERNEL_DEFAULT_PRIORITY_USER
#define HIGH_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER - 2)

#define MAX_WAITERS 6
#define MAX_ROUNDS 8

enum State { WAITING = 1, HOLDING, DONE };

typedef struct Waiter {
    int write;
    int priority;
} Waiter;

static SceUID rwlock, release;
static const Waiter *waiters;
static SceUID threads[MAX_WAITERS];
static volatile int state[MAX_WAITERS], got_round[MAX_WAITERS], lock_result[MAX_WAITERS];
static volatile int current_round;
static int released[MAX_WAITERS];

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

// Takes the lock, notes the round it got it in, and keeps it until the test releases it
static int locker(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    state[i] = WAITING;
    const int write = waiters[i].write;
    lock_result[i] = write ? sceKernelLockWriteRWLock(rwlock, &timeout) : sceKernelLockReadRWLock(rwlock, &timeout);
    if (lock_result[i] == 0) {
        got_round[i] = current_round;
        state[i] = HOLDING;
        timeout = TEST_WAIT_US;
        sceKernelWaitSema(release, 1, &timeout);
        write ? sceKernelUnlockWriteRWLock(rwlock) : sceKernelUnlockReadRWLock(rwlock);
    }
    state[i] = DONE;
    return 0;
}

// Whether every waiter is in the wait its state says, and every released one is done
static int settled(int count) {
    for (int i = 0; i < count; i++) {
        if (state[i] == DONE)
            continue;
        if (released[i])
            return 0;
        if (state[i] == WAITING && !waits_in(threads[i], WAIT_TYPE_RW_LOCK))
            return 0;
        if (state[i] == HOLDING && !waits_in(threads[i], WAIT_TYPE_SEMA))
            return 0;
    }
    return 1;
}

// Waits until settled holds on 20 polls in a row, at most 5 s. A thread that was just woken may
// show its wait until it runs, so one look isn't enough.
static void wait_settled(int count) {
    int stable = 0;
    for (int poll = 0; poll < 5000 && stable < 20; poll++) {
        stable = settled(count) ? stable + 1 : 0;
        if (stable < 20)
            sceKernelDelayThread(1000);
    }
}

static int holders(int count) {
    int n = 0;
    for (int i = 0; i < count; i++)
        n += state[i] == HOLDING;
    return n;
}

// Queues the waiters in order behind a lock this thread holds, for writing or reading, then
// unlocks it and lets each round of holders go until none is left
static void run(const char *name, SceUInt32 attr, int main_writes, const Waiter *list, int count) {
    rwlock = sceKernelCreateRWLock("rwlock", attr, NULL);
    release = sceKernelCreateSema("release", 0, 0, MAX_WAITERS, NULL);
    waiters = list;
    current_round = 0;
    main_writes ? sceKernelLockWriteRWLock(rwlock, NULL) : sceKernelLockReadRWLock(rwlock, NULL);
    for (int i = 0; i < count; i++) {
        state[i] = got_round[i] = 0;
        lock_result[i] = 0xDEAD;
        released[i] = 0;
        threads[i] = sceKernelCreateThread("waiter", locker, list[i].priority, 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }

    SceKernelRWLockInfo info = { .size = sizeof(info) };
    sceKernelGetRWLockInfo(rwlock, &info);
    PRINTF(info.numReadWaitThreads, "%s: waiting readers", name);
    PRINTF(info.numWriteWaitThreads, "%s: waiting writers", name);

    current_round = 1;
    main_writes ? sceKernelUnlockWriteRWLock(rwlock) : sceKernelUnlockReadRWLock(rwlock);
    for (int round = 1; round <= MAX_ROUNDS; round++) {
        wait_settled(count);
        const int n = holders(count);
        if (n == 0)
            break;
        for (int i = 0; i < count; i++)
            released[i] = state[i] == HOLDING;
        current_round = round + 1;
        sceKernelSignalSema(release, n);
    }
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);

    for (int i = 0; i < count; i++) {
        const char *kind = list[i].write ? "writer" : "reader";
        if (lock_result[i] == 0)
            PRINTF(got_round[i], "%s: waiter %d, a %s, gets the lock in round", name, i + 1, kind);
        else
            PRINTF(lock_result[i], "%s: waiter %d, a %s, doesn't get the lock", name, i + 1, kind);
    }
    sceKernelDeleteSema(release);
    sceKernelDeleteRWLock(rwlock);
}

int main(void) {
    // Same priorities, so the order is the queue order
    static const Waiter fifo_mixed[] = {
        { 1, MID_PRIORITY }, { 0, MID_PRIORITY }, { 0, MID_PRIORITY }, { 1, MID_PRIORITY }, { 0, MID_PRIORITY },
    };
    run("FIFO, write unlock, writer first", 0, 1, fifo_mixed, 5);
    run("FIFO, read unlock, writer first", 0, 0, fifo_mixed, 5);
    static const Waiter fifo_readers_first[] = {
        { 0, MID_PRIORITY }, { 1, MID_PRIORITY }, { 0, MID_PRIORITY }, { 1, MID_PRIORITY },
    };
    run("FIFO, write unlock, reader first", 0, 1, fifo_readers_first, 4);
    static const Waiter fifo_writers[] = {
        { 1, MID_PRIORITY }, { 1, MID_PRIORITY }, { 1, MID_PRIORITY },
    };
    run("FIFO, write unlock, only writers", 0, 1, fifo_writers, 3);

    // The priorities rise against the queue order where it matters
    static const Waiter prio_mixed[] = {
        { 1, LOW_PRIORITY }, { 0, LOW_PRIORITY }, { 1, HIGH_PRIORITY }, { 0, MID_PRIORITY }, { 0, HIGH_PRIORITY },
    };
    run("priority, write unlock, high writer queued third", SCE_KERNEL_ATTR_THREAD_PRIO, 1, prio_mixed, 5);
    run("priority, read unlock, high writer queued third", SCE_KERNEL_ATTR_THREAD_PRIO, 0, prio_mixed, 5);
    static const Waiter prio_readers_high[] = {
        { 1, MID_PRIORITY }, { 0, HIGH_PRIORITY }, { 1, LOW_PRIORITY }, { 0, LOW_PRIORITY },
    };
    run("priority, write unlock, high reader behind a writer", SCE_KERNEL_ATTR_THREAD_PRIO, 1, prio_readers_high, 4);
    static const Waiter prio_writers[] = {
        { 1, LOW_PRIORITY }, { 1, MID_PRIORITY }, { 1, HIGH_PRIORITY },
    };
    run("priority, write unlock, only writers", SCE_KERNEL_ATTR_THREAD_PRIO, 1, prio_writers, 3);

    test_exit();
    return 0;
}
