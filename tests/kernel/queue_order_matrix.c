// The order each kind of wait queue wakes three waiters in, for each attribute that could order it:
// semaphore, event flag, mutex, lightweight mutex, rwlock writers, condition, lightweight
// condition and auto reset simple event, each FIFO (0) and with 0x2000, and message pipe receivers
// with each of the attribute bits 0x1, 0x2, 0x4 and 0x8. The waiters queue in the order 0, 1, 2,
// each once the one before waits, either at the priorities 0x90, 0x70, 0x80 or all at 0x80, and
// once more at 0x90, 0x70, 0x80 with waiter 2 raised to 0x60 while all three wait. Each
// release lets one waiter through: a signal of 1, a set of a bit the waiters clear, a send of one
// byte, or for the locks an unlock, where each waiter unlocks again after it got the lock. The order
// prints as one hex digit per waiter, first woken first, so 0x012 is FIFO order and 0x120 is
// priority order for the first set of priorities.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

typedef enum Kind { SEMA, FLAG, MUTEX, LW_MUTEX, RW_WRITE, COND, LW_COND, EVENT, PIPE } Kind;

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
        if ((result[i] = sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &bits, &timeout)) == 0)
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
        sceKernelLockMutex(mutex, 1, NULL);
        if ((result[i] = sceKernelWaitCond(object, &timeout)) == 0)
            woke(i);
        sceKernelUnlockMutex(mutex, 1);
        break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        if ((result[i] = sceKernelWaitLwCond(&lw_cond, &timeout)) == 0)
            woke(i);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case EVENT:
        if ((result[i] = sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout)) == 0)
            woke(i);
        break;
    case PIPE:
        if ((result[i] = sceKernelReceiveMsgPipe(object, &byte, 1, MSG_PIPE_MODE_FULL, NULL, &timeout)) == 0)
            woke(i);
        break;
    }
    return 0;
}

static void create(Kind k, SceUInt32 attr) {
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", attr, 0, 3, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", attr | SCE_EVENT_WAITMULTIPLE, 0, NULL); break;
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
    case LW_COND:
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lwcond", attr, &lw_mutex, NULL);
        break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", attr | EVENT_ATTR_AUTO_RESET, 0, NULL); break;
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
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(mutex);
        break;
    case LW_COND:
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
    case LW_COND: sceKernelSignalLwCond(&lw_cond); break;
    case EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    case PIPE: sceKernelTrySendMsgPipe(object, &byte, 1, MSG_PIPE_MODE_FULL, &size); break;
    }
}

static int is_lock(Kind k) {
    return k == MUTEX || k == LW_MUTEX || k == RW_WRITE;
}

static void run(Kind k, const char *name, SceUInt32 attr, const int *priorities, const char *priority_name, int raise_last) {
    kind = k;
    done_count = 0;
    for (int i = 0; i < 3; i++) {
        order[i] = 0xF;
        result[i] = 0xDEAD;
    }
    create(k, attr);
    SceUID threads[3];
    for (int i = 0; i < 3; i++) {
        threads[i] = sceKernelCreateThread("waiter", waiter, priorities[i], 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }
    if (raise_last)
        sceKernelChangeThreadPriority(threads[2], 0x60);
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
    PRINTF((order[0] << 8) | (order[1] << 4) | order[2], "%s, attr 0x%X, priorities %s: wake order", name, (unsigned)attr, priority_name);
    if (done_count != 3)
        PRINTF(done_count, "%s, attr 0x%X, priorities %s: waiters woken", name, (unsigned)attr, priority_name);
}

int main(void) {
    static const int mixed[3] = { 0x90, 0x70, 0x80 };
    static const int same[3] = { 0x80, 0x80, 0x80 };
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
    };
    static const SceUInt32 attrs[] = { 0, SCE_KERNEL_ATTR_THREAD_PRIO };
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        for (unsigned a = 0; a < 2; a++) {
            run(kinds[k].kind, kinds[k].name, attrs[a], mixed, "0x90 0x70 0x80", 0);
            run(kinds[k].kind, kinds[k].name, attrs[a], same, "all 0x80", 0);
            run(kinds[k].kind, kinds[k].name, attrs[a], mixed, "0x90 0x70 0x80, then waiter 2 raised to 0x60", 1);
        }
    }
    static const SceUInt32 pipe_attrs[] = { 0, 0x1, 0x2, 0x4, 0x8 };
    for (unsigned a = 0; a < sizeof(pipe_attrs) / sizeof(pipe_attrs[0]); a++) {
        run(PIPE, "message pipe receivers", pipe_attrs[a], mixed, "0x90 0x70 0x80", 0);
        run(PIPE, "message pipe receivers", pipe_attrs[a], same, "all 0x80", 0);
        run(PIPE, "message pipe receivers", pipe_attrs[a], mixed, "0x90 0x70 0x80, then waiter 2 raised to 0x60", 1);
    }

    test_exit();
    return 0;
}
