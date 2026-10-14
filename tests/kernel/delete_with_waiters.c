// Deleting each sync object while 2 threads wait on it, the first in a plain wait and the second in
// a wait that runs callbacks: what the delete returns, what each waiter gets back, and whether the
// time left is written back. Waiters of condition variables also print whether they hold the mutex
// again after. For a reader/writer lock the first waiter reads and the second writes. Simple events
// and timers are last, so a hang on them loses nothing else.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { SEMA, EVENT_FLAG, MUTEX, COND, LW_MUTEX, LW_COND, RWLOCK, MSG_PIPE, SIMPLE_EVENT, TIMER, KIND_COUNT };
static const char *const kind_names[] = {
    "semaphore", "event flag", "mutex", "condition variable", "lightweight mutex", "lightweight condition variable",
    "reader/writer lock", "message pipe", "simple event", "timer",
};

static volatile int kind;
static SceUID id, cond_mutex;
static SceKernelLwMutexWork lw_mutex, lw_cond_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int results[2], unlocks[2];
static SceUInt left[2];

// Waits on the object of the current kind, with callbacks in slot 1
static int waiter(SceSize args, void *argp) {
    (void)args;
    const int slot = *(const int *)argp;
    const int cb = slot == 1;
    SceUInt timeout = TEST_WAIT_US;
    unsigned int bits;
    SceUInt32 pattern;
    SceUInt64 user_data;
    char buf[4];
    int result = 0;
    switch (kind) {
    case SEMA: result = cb ? sceKernelWaitSemaCB(id, 1, &timeout) : sceKernelWaitSema(id, 1, &timeout); break;
    case EVENT_FLAG:
        result = cb ? sceKernelWaitEventFlagCB(id, 0x1, SCE_EVENT_WAITAND, &bits, &timeout)
                    : sceKernelWaitEventFlag(id, 0x1, SCE_EVENT_WAITAND, &bits, &timeout);
        break;
    case MUTEX: result = cb ? sceKernelLockMutexCB(id, 1, &timeout) : sceKernelLockMutex(id, 1, &timeout); break;
    case COND:
        sceKernelLockMutex(cond_mutex, 1, NULL);
        result = cb ? sceKernelWaitCondCB(id, &timeout) : sceKernelWaitCond(id, &timeout);
        unlocks[slot] = sceKernelUnlockMutex(cond_mutex, 1);
        break;
    case LW_MUTEX:
        result = cb ? sceKernelLockLwMutexCB(&lw_mutex, 1, &timeout) : sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_cond_mutex, 1, NULL);
        result = cb ? sceKernelWaitLwCondCB(&lw_cond, &timeout) : sceKernelWaitLwCond(&lw_cond, &timeout);
        unlocks[slot] = sceKernelUnlockLwMutex(&lw_cond_mutex, 1);
        break;
    case RWLOCK: result = cb ? sceKernelLockWriteRWLockCB(id, &timeout) : sceKernelLockReadRWLock(id, &timeout); break;
    case MSG_PIPE:
        result = cb ? sceKernelReceiveMsgPipeCB(id, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout)
                    : sceKernelReceiveMsgPipe(id, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout);
        break;
    case SIMPLE_EVENT:
        result = cb ? sceKernelWaitEventCB(id, EVENT_BIT_A, &pattern, &user_data, &timeout)
                    : sceKernelWaitEvent(id, EVENT_BIT_A, &pattern, &user_data, &timeout);
        break;
    case TIMER:
        result = cb ? sceKernelWaitEventCB(id, EVENT_TIMER, &pattern, &user_data, &timeout)
                    : sceKernelWaitEvent(id, EVENT_TIMER, &pattern, &user_data, &timeout);
        break;
    }
    left[slot] = timeout;
    results[slot] = result;
    return 0;
}

// Creates the object of the kind, held by this thread where it is a lock
static void create(void) {
    switch (kind) {
    case SEMA: id = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case EVENT_FLAG: id = sceKernelCreateEventFlag("flag", SCE_EVENT_WAITMULTIPLE, 0, NULL); break;
    case MUTEX: id = sceKernelCreateMutex("mutex", 0, 1, NULL); break;
    case COND:
        cond_mutex = sceKernelCreateMutex("cond mutex", 0, 0, NULL);
        id = sceKernelCreateCond("cond", 0, cond_mutex, NULL);
        break;
    case LW_MUTEX: sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 1, NULL); break;
    case LW_COND:
        sceKernelCreateLwMutex(&lw_cond_mutex, "lw cond mutex", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lw cond", 0, &lw_cond_mutex, NULL);
        break;
    case RWLOCK:
        id = sceKernelCreateRWLock("rwlock", 0, NULL);
        sceKernelLockWriteRWLock(id, NULL);
        break;
    case MSG_PIPE: id = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    case SIMPLE_EVENT: id = sceKernelCreateSimpleEvent("event", 0, 0, NULL); break;
    case TIMER: id = sceKernelCreateTimer("timer", 0, NULL); break;
    }
}

static int delete(void) {
    switch (kind) {
    case SEMA: return sceKernelDeleteSema(id);
    case EVENT_FLAG: return sceKernelDeleteEventFlag(id);
    case MUTEX: return sceKernelDeleteMutex(id);
    case COND: return sceKernelDeleteCond(id);
    case LW_MUTEX: return sceKernelDeleteLwMutex(&lw_mutex);
    case LW_COND: return sceKernelDeleteLwCond(&lw_cond);
    case RWLOCK: return sceKernelDeleteRWLock(id);
    case MSG_PIPE: return sceKernelDeleteMsgPipe(id);
    case SIMPLE_EVENT: return sceKernelDeleteSimpleEvent(id);
    case TIMER: return sceKernelDeleteTimer(id);
    }
    return 0;
}

// 0 if no time is left, 1 if some is, 2 if all of it is
static int time_left(SceUInt value) {
    return value == 0 ? 0 : value >= TEST_WAIT_US ? 2 : 1;
}

int main(void) {
    static const char *const waiter_names[] = { "the plain waiter", "the waiter with callbacks" };
    for (kind = 0; kind < KIND_COUNT; kind++) {
        const char *name = kind_names[kind];
        SceUID threads[2];
        create();
        for (int i = 0; i < 2; i++) {
            results[i] = unlocks[i] = -1;
            left[i] = 0xDEAD;
            threads[i] = test_start_thread_arg(waiter, i);
            test_wait_until_waiting(threads[i]);
        }
        const int deleted = delete();
        PRINTF(deleted, "%s: delete with 2 waiters", name);
        for (int i = 0; i < 2; i++) {
            test_join_thread(threads[i]);
            PRINTF(results[i], "%s: %s", name, waiter_names[i]);
            PRINTF(time_left(left[i]), "%s: the time left of %s", name, waiter_names[i]);
            if (kind == COND || kind == LW_COND)
                PRINTF(unlocks[i], "%s: %s unlocks the mutex after", name, waiter_names[i]);
        }
        // What the delete left behind
        if (kind == COND)
            sceKernelDeleteMutex(cond_mutex);
        if (kind == LW_COND) {
            if (deleted != 0)
                sceKernelDeleteLwCond(&lw_cond);
            sceKernelDeleteLwMutex(&lw_cond_mutex);
        }
        if (kind == LW_MUTEX && deleted != 0) {
            sceKernelUnlockLwMutex(&lw_mutex, 1);
            sceKernelDeleteLwMutex(&lw_mutex);
        }
    }

    test_exit();
    return 0;
}
