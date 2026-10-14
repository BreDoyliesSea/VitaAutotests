// Whether a lock owner runs at the priority of a higher priority thread waiting for it. Owner L
// holds the lock at a low priority on core 0, waiter H at a high priority waits for it, and a
// middle priority thread M on core 0 becomes ready and spins without syscalls for a bounded time.
// If L keeps its own priority, M runs before L can unlock and H waits behind M. Covered: a mutex,
// a mutex with the priority attribute, a lightweight mutex, and a rwlock write lock. Also what
// thread info reports as L's current priority while H waits. The main thread runs on core 1. The
// threads record their steps: L records 1 when it unlocks, M records 2 when it ends its spin, H
// records 3 once it has the lock, so 1 3 2 means L ran at H's priority and 2 1 3 means it didn't.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define HIGH_PRIORITY 0x70
#define MIDDLE_PRIORITY 0x90
#define LOW_PRIORITY 0xB0
#define MAX_STEPS 4
#define SPINS 100000000

typedef enum Kind { MUTEX, LW_MUTEX, RWLOCK } Kind;

static Kind kind;
static SceUID lock;
static SceKernelLwMutexWork lw_mutex;
static volatile int l_locked, l_go, m_go, h_result;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static void do_lock(void) {
    switch (kind) {
    case MUTEX: sceKernelLockMutex(lock, 1, NULL); break;
    case LW_MUTEX: sceKernelLockLwMutex(&lw_mutex, 1, NULL); break;
    case RWLOCK: sceKernelLockWriteRWLock(lock, NULL); break;
    }
}

static void do_unlock(void) {
    switch (kind) {
    case MUTEX: sceKernelUnlockMutex(lock, 1); break;
    case LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case RWLOCK: sceKernelUnlockWriteRWLock(lock); break;
    }
}

// Takes the lock, waits until told, then needs a little CPU time before it unlocks
static int l_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    do_lock();
    l_locked = 1;
    for (volatile int n = 0; n < SPINS && !l_go; n++)
        ;
    for (volatile int n = 0; n < 1000; n++)
        ;
    record(1);
    do_unlock();
    return 0;
}

static int m_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (volatile int n = 0; n < SPINS && !m_go; n++)
        ;
    record(2);
    return 0;
}

static int h_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    switch (kind) {
    case MUTEX: h_result = sceKernelLockMutex(lock, 1, &timeout); break;
    case LW_MUTEX: h_result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout); break;
    case RWLOCK: h_result = sceKernelLockWriteRWLock(lock, &timeout); break;
    }
    record(3);
    if (h_result == 0)
        do_unlock();
    return 0;
}

static int current_priority(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.currentPriority;
}

static void run(Kind k, SceUInt32 attr, const char *name) {
    kind = k;
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
    l_locked = l_go = m_go = 0;
    h_result = 0xDEAD;
    switch (k) {
    case MUTEX: lock = sceKernelCreateMutex("mutex", attr, 0, NULL); break;
    case LW_MUTEX: sceKernelCreateLwMutex(&lw_mutex, "lw", attr, 0, NULL); break;
    case RWLOCK: lock = sceKernelCreateRWLock("rw", attr, NULL); break;
    }
    const SceUID l = sceKernelCreateThread("l", l_entry, LOW_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(l, 0, NULL);
    TEST_WAIT_FOR(l_locked);
    // H waits for the lock from core 2, so it doesn't compete for core 0
    const SceUID h = sceKernelCreateThread("h", h_entry, HIGH_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    sceKernelStartThread(h, 0, NULL);
    test_wait_until_waiting(h);
    const int l_priority = current_priority(l);
    const SceUID m = sceKernelCreateThread("m", m_entry, MIDDLE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(m, 0, NULL);
    // L may now unlock, if it gets core 0. M spins until it is told to stop after 300 ms.
    l_go = 1;
    sceKernelDelayThread(300000);
    m_go = 1;
    sceKernelWaitThreadEnd(l, NULL, NULL);
    sceKernelWaitThreadEnd(m, NULL, NULL);
    sceKernelWaitThreadEnd(h, NULL, NULL);
    sceKernelDeleteThread(l);
    sceKernelDeleteThread(m);
    sceKernelDeleteThread(h);
    switch (k) {
    case MUTEX: sceKernelDeleteMutex(lock); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case RWLOCK: sceKernelDeleteRWLock(lock); break;
    }
    PRINTF(l_priority, "%s: the owner's current priority while the waiter waits", name);
    PRINTF(h_result, "%s: the waiter's result", name);
    const int count = step_count < MAX_STEPS ? step_count : MAX_STEPS;
    for (int i = 0; i < count; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    run(MUTEX, 0, "mutex");
    run(MUTEX, SCE_KERNEL_ATTR_THREAD_PRIO, "priority mutex");
    run(LW_MUTEX, 0, "lightweight mutex");
    run(LW_MUTEX, SCE_KERNEL_ATTR_THREAD_PRIO, "priority lightweight mutex");
    run(RWLOCK, 0, "rwlock write lock");

    test_exit();
    return 0;
}
