// What the rwlock attribute 0x4 changes, next to 0, 0x2000 and 0x2004: whether a read lock is
// granted while a writer waits, to another thread and to the holder of a read lock, and in which
// rounds waiters get the lock once a write lock is released. In the rounds case reader 0, writer 1
// and reader 2 queue in that order behind the main thread's write lock, all at one priority. Each
// waiter keeps the lock until the round ends, and the round each one got the lock in prints as one
// hex digit per waiter, 0 meaning never.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAX_ROUNDS 4

static SceUID rwlock;
static volatile int role[3], got[3], release[3], ended[3], round_now;

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    const int r = role[i] ? sceKernelLockWriteRWLock(rwlock, &timeout) : sceKernelLockReadRWLock(rwlock, &timeout);
    if (r == 0) {
        got[i] = round_now;
        TEST_WAIT_FOR(release[i]);
        if (role[i])
            sceKernelUnlockWriteRWLock(rwlock);
        else
            sceKernelUnlockReadRWLock(rwlock);
    }
    ended[i] = 1;
    return 0;
}

static SceUID threads[3];

static void start_waiter(int i, int write) {
    role[i] = write;
    got[i] = release[i] = ended[i] = 0;
    threads[i] = sceKernelCreateThread("waiter", waiter, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceKernelStartThread(threads[i], sizeof(i), &i);
    test_wait_until_waiting(threads[i]);
}

// Waits until the set of waiters that got the lock stayed the same for 20 polls in a row
static void settle(int count) {
    int last = -1, stable = 0;
    for (int poll = 0; poll < 3000 && stable < 20; poll++) {
        int now = 0;
        for (int i = 0; i < count; i++)
            now = (now << 1) | (got[i] != 0);
        stable = now == last ? stable + 1 : 0;
        last = now;
        sceKernelDelayThread(1000);
    }
}

static int try_read_result;

static int try_reader(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    try_read_result = sceKernelTryLockReadRWLock(rwlock);
    if (try_read_result == 0)
        sceKernelUnlockReadRWLock(rwlock);
    return 0;
}

static void read_while_writer_waits(SceUInt32 attr) {
    rwlock = sceKernelCreateRWLock("rw", attr, NULL);
    sceKernelLockReadRWLock(rwlock, NULL);
    round_now = 1;
    start_waiter(0, 1);
    try_read_result = 0xDEAD;
    const SceUID other = sceKernelCreateThread("try", try_reader, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceKernelStartThread(other, 0, NULL);
    test_join_thread(other);
    const int own = sceKernelTryLockReadRWLock(rwlock);
    if (own == 0)
        sceKernelUnlockReadRWLock(rwlock);
    sceKernelUnlockReadRWLock(rwlock);
    TEST_WAIT_FOR(got[0]);
    release[0] = 1;
    test_join_thread(threads[0]);
    sceKernelDeleteRWLock(rwlock);
    PRINTF(try_read_result, "attr 0x%X, a read lock held and a writer waiting: another thread's try read lock", (unsigned)attr);
    PRINTF(own, "attr 0x%X, a read lock held and a writer waiting: the holder's try read lock", (unsigned)attr);
}

static void rounds(SceUInt32 attr) {
    rwlock = sceKernelCreateRWLock("rw", attr, NULL);
    sceKernelLockWriteRWLock(rwlock, NULL);
    round_now = 1;
    start_waiter(0, 0);
    start_waiter(1, 1);
    start_waiter(2, 0);
    sceKernelUnlockWriteRWLock(rwlock);
    for (int r = 1; r <= MAX_ROUNDS; r++) {
        settle(3);
        int all = 1;
        for (int i = 0; i < 3; i++)
            all &= got[i] != 0;
        if (all)
            break;
        round_now = r + 1;
        // Ends the round: the waiters that hold the lock let it go
        for (int i = 0; i < 3; i++) {
            if (got[i] && !release[i]) {
                release[i] = 1;
                TEST_WAIT_FOR(ended[i]);
            }
        }
    }
    for (int i = 0; i < 3; i++)
        release[i] = 1;
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);
    sceKernelDeleteRWLock(rwlock);
    PRINTF((got[0] << 8) | (got[1] << 4) | got[2], "attr 0x%X, reader 0, writer 1, reader 2 behind a write lock: rounds", (unsigned)attr);
}

int main(void) {
    static const SceUInt32 attrs[] = { 0, 0x4, SCE_KERNEL_ATTR_THREAD_PRIO, SCE_KERNEL_ATTR_THREAD_PRIO | 0x4 };
    for (unsigned a = 0; a < sizeof(attrs) / sizeof(attrs[0]); a++) {
        read_while_writer_waits(attrs[a]);
        rounds(attrs[a]);
    }
    test_exit();
    return 0;
}
