// Releases that a lone object would reject, made while threads wait on the object: whether the
// call still fails, and whether it wakes a waiter first. The cases:
// - a semaphore with a maximum of 1 and a count of 0, a waiter for 1, a signal of 2
// - a semaphore with a maximum of 2 and a count of 1, a waiter for 2, a signal of 2, which goes
//   past the maximum but leaves 1 once the waiter took its 2
// - a semaphore with a maximum of 1 and a count of 0, two waiters for 1, a signal of 2, which both
//   waiters could take
// - a semaphore with a maximum of 2 and two waiters, canceled with a count of 3
// - a recursive mutex the main thread holds once, a waiter, an unlock of 2
// - a mutex another thread O holds, a waiter, an unlock by the main thread
// - a recursive lightweight mutex the main thread holds once, a waiter, an unlock of 2
// - a lightweight mutex O holds, a waiter, an unlock by the main thread
// - a rwlock O holds a read lock of, a writer waiting, a read unlock by the main thread
// Every thread is pinned to core 0 and the waiters run below the main thread, so none of them
// can run between the call and the reads after it. Each case prints the call's result, whether
// each waiter still waits right after it, and the count the object reports. Then the object is
// deleted, which ends the waits left, and the case prints each waiter's result.
//
// On a PS Vita TV on firmware 3.65 every release a lone object would refuse is refused with waiters
// too, and wakes no one: a signal past the maximum gives 0x80028103 even when the waiters would
// take the count back under it, a cancel past the maximum 0x8002000F, an unlock of more than a
// recursive mutex holds 0x80028144 (0x80028184 for a lightweight one), and an unlock by a thread
// that doesn't own it 0x80028146 (0x80028186). A read unlock by a thread that holds no read lock
// works, though: it takes one count off the lock, which hands it to the waiting writer, so read
// locks have no owner.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0

typedef enum Kind {
    SEMA_1,
    SEMA_2,
    MUTEX,
    LW_MUTEX,
    WRITER,
    OWNER_MUTEX,
    OWNER_LW_MUTEX,
    OWNER_READ
} Kind;

static Kind kind;
static SceUID object, gate;
static SceKernelLwMutexWork lw_mutex;
static volatile int result[2], owner_up;

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    switch (kind) {
    case SEMA_1: result[i] = sceKernelWaitSema(object, 1, &timeout); break;
    case SEMA_2: result[i] = sceKernelWaitSema(object, 2, &timeout); break;
    case MUTEX:
        result[i] = sceKernelLockMutex(object, 1, &timeout);
        if (result[i] == 0)
            sceKernelUnlockMutex(object, 1);
        break;
    case LW_MUTEX:
        result[i] = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        if (result[i] == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case WRITER:
        result[i] = sceKernelLockWriteRWLock(object, &timeout);
        if (result[i] == 0)
            sceKernelUnlockWriteRWLock(object);
        break;
    default: break;
    }
    return 0;
}

// Takes the object the way kind says, waits for the gate, and lets go of it
static int owner(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    switch (kind) {
    case OWNER_MUTEX: sceKernelLockMutex(object, 1, NULL); break;
    case OWNER_LW_MUTEX: sceKernelLockLwMutex(&lw_mutex, 1, NULL); break;
    case OWNER_READ: sceKernelLockReadRWLock(object, NULL); break;
    default: break;
    }
    owner_up = 1;
    SceUInt timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(gate, 1, &timeout);
    switch (kind) {
    case OWNER_MUTEX: sceKernelUnlockMutex(object, 1); break;
    case OWNER_LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case OWNER_READ: sceKernelUnlockReadRWLock(object); break;
    default: break;
    }
    return 0;
}

static SceUID threads[2], owner_thread;

static SceUID start(SceKernelThreadEntry entry, int i) {
    const SceUID thread = sceKernelCreateThread(
        "t", entry, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    return thread;
}

// Starts the owner if the case has one, then the waiters, each once the one before waits
static void begin(Kind owner_kind, Kind waiter_kind, int count) {
    owner_thread = -1;
    if (owner_kind != waiter_kind) {
        kind = owner_kind;
        owner_up = 0;
        owner_thread = start(owner, 0);
        TEST_WAIT_FOR(owner_up);
        test_wait_until_waiting(owner_thread);
    }
    kind = waiter_kind;
    for (int i = 0; i < count; i++) {
        result[i] = 0xDEAD;
        threads[i] = start(waiter, i);
        test_wait_until_waiting(threads[i]);
    }
}

static int waits(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return (info.status & SCE_THREAD_WAITING) != 0;
}

static void finish(
    const char *name, int call, int count, int object_count, const char *count_name) {
    int still[2];
    for (int i = 0; i < count; i++)
        still[i] = waits(threads[i]);
    PRINTF(call, "%s: the call", name);
    for (int i = 0; i < count; i++)
        PRINTF(still[i], "%s: waiter %d still waits right after it", name, i);
    PRINTF(object_count, "%s: %s", name, count_name);
}

static void join(const char *name, int count) {
    if (owner_thread >= 0) {
        sceKernelSignalSema(gate, 1);
        test_join_thread(owner_thread);
    }
    for (int i = 0; i < count; i++) {
        test_join_thread(threads[i]);
        PRINTF(result[i], "%s: waiter %d's result", name, i);
    }
}

static int sema_count(void) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(object, &info);
    return info.currentCount;
}

static void sema_case(const char *name, int init, int max, Kind k, int count, int signal) {
    object = sceKernelCreateSema("sema", 0, init, max, NULL);
    begin(k, k, count);
    const int call = sceKernelSignalSema(object, signal);
    const int after = sema_count();
    finish(name, call, count, after, "the count after it");
    sceKernelDeleteSema(object);
    join(name, count);
}

static void sema_cancel_case(void) {
    const char *name = "cancel to 3 with a maximum of 2";
    object = sceKernelCreateSema("sema", 0, 0, 2, NULL);
    begin(SEMA_1, SEMA_1, 2);
    int waiting = 0xDEAD;
    const int call = sceKernelCancelSema(object, 3, &waiting);
    const int after = sema_count();
    finish(name, call, 2, after, "the count after it");
    PRINTF(waiting, "%s: the waiters it counted", name);
    sceKernelDeleteSema(object);
    join(name, 2);
}

static int mutex_count(void) {
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(object, (SceKernelMutexInfo *)&info);
    return info.currentCount;
}

static int lw_mutex_count(void) {
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    return info.currentCount;
}

static void mutex_cases(void) {
    const char *name = "recursive mutex held once, unlock of 2";
    object = sceKernelCreateMutex("mutex", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 1, NULL);
    begin(MUTEX, MUTEX, 1);
    int call = sceKernelUnlockMutex(object, 2);
    finish(name, call, 1, mutex_count(), "the count after it");
    sceKernelDeleteMutex(object);
    join(name, 1);

    name = "mutex held by another thread, unlock";
    object = sceKernelCreateMutex("mutex", 0, 0, NULL);
    begin(OWNER_MUTEX, MUTEX, 1);
    call = sceKernelUnlockMutex(object, 1);
    finish(name, call, 1, mutex_count(), "the count after it");
    sceKernelDeleteMutex(object);
    join(name, 1);

    name = "recursive lightweight mutex held once, unlock of 2";
    sceKernelCreateLwMutex(&lw_mutex, "lw", LW_MUTEX_ATTR_RECURSIVE, 1, NULL);
    begin(LW_MUTEX, LW_MUTEX, 1);
    call = sceKernelUnlockLwMutex(&lw_mutex, 2);
    finish(name, call, 1, lw_mutex_count(), "the count after it");
    sceKernelDeleteLwMutex(&lw_mutex);
    join(name, 1);

    name = "lightweight mutex held by another thread, unlock";
    sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
    begin(OWNER_LW_MUTEX, LW_MUTEX, 1);
    call = sceKernelUnlockLwMutex(&lw_mutex, 1);
    finish(name, call, 1, lw_mutex_count(), "the count after it");
    sceKernelDeleteLwMutex(&lw_mutex);
    join(name, 1);
}

static void rwlock_case(void) {
    const char *name = "read lock held by another thread, read unlock";
    object = sceKernelCreateRWLock("rw", 0, NULL);
    begin(OWNER_READ, WRITER, 1);
    const int call = sceKernelUnlockReadRWLock(object);
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    sceKernelGetRWLockInfo(object, &info);
    finish(name, call, 1, info.lockCount, "the lock count after it");
    sceKernelDeleteRWLock(object);
    join(name, 1);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);

    sema_case("maximum 1, a waiter for 1, signal 2", 0, 1, SEMA_1, 1, 2);
    sema_case("maximum 2 and a count of 1, a waiter for 2, signal 2", 1, 2, SEMA_2, 1, 2);
    sema_case("maximum 1, two waiters for 1, signal 2", 0, 1, SEMA_1, 2, 2);
    sema_cancel_case();
    mutex_cases();
    rwlock_case();

    sceKernelDeleteSema(gate);
    test_exit();
    return 0;
}
