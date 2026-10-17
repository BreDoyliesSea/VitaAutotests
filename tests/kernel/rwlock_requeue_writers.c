// Who gets a reader/writer lock first after a priority change moves its waiters around. Each
// waiter records its turn once it has the lock, and then unlocks, so the turns show the order the
// lock went round in. Cases:
// - the main thread holds the write lock, writer W1, reader R and writer W2 wait at falling
//   priorities, then W1 drops below W2, and the main thread unlocks
// - the same with a FIFO queue
// - the main thread holds a read lock, writer W waits, then reader R behind it, and R is raised
//   above W. The test prints whether R got its read lock within 500 ms of the change, and then
//   the main thread unlocks
//
// On a PS Vita TV on firmware 3.65 the waiting writers get the lock before the reader in every
// case, whatever the queue order: W2, W1 and then R once W1 drops below W2, W1, W2 and then R in
// the FIFO queue, and W and then R once R is raised above W, which doesn't get R a read lock
// either. So a waiting writer goes before every waiting reader, and a priority change only orders
// the writers among themselves.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define HIGH 0x78
#define LOW 0x98

static SceUID rwlock;
static SceUID threads[3];
static int writes[3];
static volatile int result[3], turn[3], turns, got[3];

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    if (writes[i]) {
        result[i] = sceKernelLockWriteRWLock(rwlock, &timeout);
        turn[i] = __atomic_add_fetch(&turns, 1, __ATOMIC_SEQ_CST);
        if (result[i] == 0)
            sceKernelUnlockWriteRWLock(rwlock);
    } else {
        result[i] = sceKernelLockReadRWLock(rwlock, &timeout);
        got[i] = 1;
        turn[i] = __atomic_add_fetch(&turns, 1, __ATOMIC_SEQ_CST);
        if (result[i] == 0)
            sceKernelUnlockReadRWLock(rwlock);
    }
    return 0;
}

static void create(SceUInt32 attr, const int *write, int n) {
    rwlock = sceKernelCreateRWLock("rwlock", attr, NULL);
    turns = 0;
    for (int i = 0; i < n; i++) {
        writes[i] = write[i];
        result[i] = 0xDEAD;
        turn[i] = 0;
        got[i] = 0;
    }
}

static void start_waiter(int i, int priority) {
    threads[i] = sceKernelCreateThread("waiter", waiter, priority, 0x4000, 0, 0, NULL);
    sceKernelStartThread(threads[i], sizeof(i), &i);
    test_wait_until_waiting(threads[i]);
}

static void finish(const char *name, const char *const *names, int n) {
    TEST_WAIT_FOR(turns == n);
    for (int i = 0; i < n; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < n; i++) {
        PRINTF(turn[i], "%s: %s's turn", name, names[i]);
        PRINTF(result[i], "%s: %s's result", name, names[i]);
    }
    sceKernelDeleteRWLock(rwlock);
}

static void writers_case(const char *name, SceUInt32 attr) {
    static const char *const names[3] = { "W1", "R", "W2" };
    static const int write[3] = { 1, 0, 1 };
    static const int priorities[3] = { 0x80, 0x88, 0x90 };
    create(attr, write, 3);
    sceKernelLockWriteRWLock(rwlock, NULL);
    for (int i = 0; i < 3; i++)
        start_waiter(i, priorities[i]);
    PRINTF(sceKernelChangeThreadPriority(threads[0], LOW), "%s: W1 drops below W2", name);
    sceKernelUnlockWriteRWLock(rwlock);
    finish(name, names, 3);
}

int main(void) {
    writers_case("write lock held, priority queue", SCE_KERNEL_ATTR_THREAD_PRIO);
    writers_case("write lock held, FIFO queue", 0);

    const char *name = "read lock held, priority queue";
    static const char *const names[2] = { "W", "R" };
    static const int write[2] = { 1, 0 };
    static const int priorities[2] = { 0x80, 0x88 };
    create(SCE_KERNEL_ATTR_THREAD_PRIO, write, 2);
    sceKernelLockReadRWLock(rwlock, NULL);
    for (int i = 0; i < 2; i++)
        start_waiter(i, priorities[i]);
    const int changed = sceKernelChangeThreadPriority(threads[1], HIGH);
    for (int t = 0; t < 500 && !got[1]; t++)
        sceKernelDelayThread(1000);
    const int r_got = got[1];
    PRINTF(changed, "%s: R raised above W", name);
    PRINTF(r_got, "%s: R got its read lock after the change", name);
    sceKernelUnlockReadRWLock(rwlock);
    finish(name, names, 2);

    test_exit();
    return 0;
}
