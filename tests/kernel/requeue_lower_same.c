// Where a waiter goes in each kind of wait queue when its priority is lowered, or set to the value
// it already has, while it waits. Three waiters at 0x88 queue in the order 0, 1, 2, each once the
// one before waits, and then one change: waiter 0 drops to 0x90, waiter 0 is set to 0x88, or
// waiter 1 is set to 0x88. Kinds: semaphore, event flag, mutex, lightweight mutex, rwlock writers,
// condition, lightweight condition, auto reset simple event, message pipe receivers, and condition
// and lightweight condition waiters that were signaled while the main thread holds the mutex and
// wait to lock it again, each FIFO and with SCE_KERNEL_ATTR_THREAD_PRIO, or for the pipe with its
// receive priority bit 0x2. For the relocking waiters the attribute is the mutex's. Each release
// lets one waiter through: a signal of 1, a set of a bit the waiters clear, a send of one byte,
// or for the locks an unlock, where each waiter unlocks again after it got the lock. The order
// prints as one hex digit per waiter, first woken first, so 0x012 is the order they queued in.
//
// On a PS Vita TV on firmware 3.65 every kind with a priority queue gives 0x120 when waiter 0 drops
// or is set to its own priority, and 0x021 when waiter 1 is, so a change puts a waiter behind every
// waiter of its new priority, even when its priority stays the same. Every FIFO queue keeps 0x012.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define PRIORITY 0x88
#define LOW 0x90

typedef enum Kind {
    SEMA,
    FLAG,
    MUTEX,
    LW_MUTEX,
    RW_WRITE,
    COND,
    LW_COND,
    EVENT,
    PIPE,
    COND_MUTEX,
    LW_COND_MUTEX,
} Kind;

static Kind kind;
static SceUID object, mutex;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int done_count;
static int order[3];
static volatile int result[3];

static void woke(int i) {
    const int n = __atomic_fetch_add(&done_count, 1, __ATOMIC_SEQ_CST);
    if (n < 3)
        order[n] = i;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    SceUInt32 bits;
    char byte;
    switch (kind) {
    case SEMA:
        if ((result[i] = sceKernelWaitSema(object, 1, &timeout)) == 0)
            woke(i);
        break;
    case FLAG:
        result[i] = sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR,
                                           &bits, &timeout);
        if (result[i] == 0)
            woke(i);
        break;
    case MUTEX:
        if ((result[i] = sceKernelLockMutex(object, 1, &timeout)) == 0) {
            woke(i);
            sceKernelUnlockMutex(object, 1);
        }
        break;
    case LW_MUTEX:
        if ((result[i] = sceKernelLockLwMutex(&lw_mutex, 1, &timeout)) == 0) {
            woke(i);
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        }
        break;
    case RW_WRITE:
        if ((result[i] = sceKernelLockWriteRWLock(object, &timeout)) == 0) {
            woke(i);
            sceKernelUnlockWriteRWLock(object);
        }
        break;
    case COND:
    case COND_MUTEX:
        sceKernelLockMutex(mutex, 1, NULL);
        if ((result[i] = sceKernelWaitCond(object, &timeout)) == 0)
            woke(i);
        sceKernelUnlockMutex(mutex, 1);
        break;
    case LW_COND:
    case LW_COND_MUTEX:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        if ((result[i] = sceKernelWaitLwCond(&lw_cond, &timeout)) == 0)
            woke(i);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case EVENT:
        result[i] = sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout);
        if (result[i] == 0)
            woke(i);
        break;
    case PIPE:
        result[i] = sceKernelReceiveMsgPipe(object, &byte, 1, MSG_PIPE_MODE_FULL, NULL, &timeout);
        if (result[i] == 0)
            woke(i);
        break;
    }
    return 0;
}

static void create(Kind k, SceUInt32 attr) {
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", attr, 0, 3, NULL); break;
    case FLAG:
        object = sceKernelCreateEventFlag("flag", attr | SCE_EVENT_WAITMULTIPLE, 0, NULL);
        break;
    case MUTEX: object = sceKernelCreateMutex("mutex", attr, 1, NULL); break;
    case LW_MUTEX: sceKernelCreateLwMutex(&lw_mutex, "lw", attr, 1, NULL); break;
    case RW_WRITE:
        object = sceKernelCreateRWLock("rw", attr, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case COND:
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
        object = sceKernelCreateCond("cond", attr, mutex, NULL);
        break;
    case COND_MUTEX:
        mutex = sceKernelCreateMutex("mutex", attr, 0, NULL);
        object = sceKernelCreateCond("cond", 0, mutex, NULL);
        break;
    case LW_COND:
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lwcond", attr, &lw_mutex, NULL);
        break;
    case LW_COND_MUTEX:
        sceKernelCreateLwMutex(&lw_mutex, "lw", attr, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lwcond", 0, &lw_mutex, NULL);
        break;
    case EVENT:
        object = sceKernelCreateSimpleEvent("event", attr | EVENT_ATTR_AUTO_RESET, 0, NULL);
        break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, attr, 4096, NULL); break;
    }
}

static void destroy(Kind k) {
    switch (k) {
    case SEMA: sceKernelDeleteSema(object); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case MUTEX: sceKernelDeleteMutex(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case RW_WRITE: sceKernelDeleteRWLock(object); break;
    case COND:
    case COND_MUTEX:
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(mutex);
        break;
    case LW_COND:
    case LW_COND_MUTEX:
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_mutex);
        break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    }
}

// Lets one more waiter through, or for the locks all of them in turn
static void release(Kind k) {
    char byte = 'x';
    SceSize size;
    switch (k) {
    case SEMA: sceKernelSignalSema(object, 1); break;
    case FLAG: sceKernelSetEventFlag(object, 0x1); break;
    case MUTEX: sceKernelUnlockMutex(object, 1); break;
    case LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case RW_WRITE: sceKernelUnlockWriteRWLock(object); break;
    case COND: sceKernelSignalCond(object); break;
    case COND_MUTEX: sceKernelUnlockMutex(mutex, 1); break;
    case LW_COND: sceKernelSignalLwCond(&lw_cond); break;
    case LW_COND_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    case PIPE: sceKernelTrySendMsgPipe(object, &byte, 1, MSG_PIPE_MODE_FULL, &size); break;
    }
}

static int is_lock(Kind k) {
    return k == MUTEX || k == LW_MUTEX || k == RW_WRITE || k == COND_MUTEX || k == LW_COND_MUTEX;
}

// Returns whether thread waits, and not on a condition
static int in_mutex_wait(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0 || !(info.status & SCE_THREAD_WAITING))
        return 0;
    const SceUInt32 type = info.waitType & ~WAIT_TYPE_CB;
    return type != 0 && type != WAIT_TYPE_COND && type != WAIT_TYPE_LW_COND;
}

// Moves the condition waiters to the mutex, which this thread then holds, in the order they queued
static void move_to_mutex(Kind k, const SceUID *threads) {
    if (k == COND_MUTEX)
        sceKernelLockMutex(mutex, 1, NULL);
    else
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    for (int i = 0; i < 3; i++) {
        if (k == COND_MUTEX)
            sceKernelSignalCondTo(object, threads[i]);
        else
            sceKernelSignalLwCondTo(&lw_cond, threads[i]);
        TEST_WAIT_FOR(in_mutex_wait(threads[i]));
    }
}

static void run(Kind k, const char *name, SceUInt32 attr, int who, int priority,
                const char *change) {
    kind = k;
    done_count = 0;
    for (int i = 0; i < 3; i++) {
        order[i] = 0xF;
        result[i] = 0xDEAD;
    }
    create(k, attr);
    SceUID threads[3];
    for (int i = 0; i < 3; i++) {
        threads[i] = sceKernelCreateThread("waiter", waiter, PRIORITY, 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }
    if (k == COND_MUTEX || k == LW_COND_MUTEX)
        move_to_mutex(k, threads);
    const int changed = sceKernelChangeThreadPriority(threads[who], priority);
    if (is_lock(k)) {
        release(k);
        TEST_WAIT_FOR(done_count == 3);
    } else {
        for (int n = 0; n < 3; n++) {
            release(k);
            TEST_WAIT_FOR(done_count > n);
        }
    }
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);
    destroy(k);
    const int wake_order = (order[0] << 8) | (order[1] << 4) | order[2];
    PRINTF(wake_order, "%s, attr 0x%X, %s: wake order", name, (unsigned)attr, change);
    if (changed != 0)
        PRINTF(changed, "%s, attr 0x%X, %s: the change", name, (unsigned)attr, change);
    if (done_count != 3)
        PRINTF(done_count, "%s, attr 0x%X, %s: waiters woken", name, (unsigned)attr, change);
}

static void run_all(Kind k, const char *name, SceUInt32 attr) {
    run(k, name, attr, 0, LOW, "waiter 0 drops to 0x90");
    run(k, name, attr, 0, PRIORITY, "waiter 0 set to 0x88");
    run(k, name, attr, 1, PRIORITY, "waiter 1 set to 0x88");
}

int main(void) {
    static const struct {
        Kind kind;
        const char *name;
    } kinds[] = {
        { SEMA, "semaphore" },
        { FLAG, "event flag" },
        { MUTEX, "mutex" },
        { LW_MUTEX, "lightweight mutex" },
        { RW_WRITE, "rwlock writers" },
        { COND, "condition" },
        { LW_COND, "lightweight condition" },
        { EVENT, "auto reset simple event" },
        { COND_MUTEX, "condition waiters relocking the mutex" },
        { LW_COND_MUTEX, "lightweight condition waiters relocking the mutex" },
    };
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        run_all(kinds[k].kind, kinds[k].name, 0);
        run_all(kinds[k].kind, kinds[k].name, SCE_KERNEL_ATTR_THREAD_PRIO);
    }
    run_all(PIPE, "message pipe receivers", 0);
    run_all(PIPE, "message pipe receivers", MSG_PIPE_ATTR_RECEIVE_PRIO);

    test_exit();
    return 0;
}
