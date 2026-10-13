// Every wait that checks callbacks, run with a callback pending: when it blocks and times out, when
// it can end at once, with a timeout of 0 and with an unknown UID. Whether the callback runs in
// each case, and the wait type each one reports while it waits.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define BAD_UID 0x12345

enum Kind { DELAY, THREAD_END, EVENT_FLAG, SEMA, MUTEX, READ_LOCK, WRITE_LOCK, COND, LW_MUTEX, LW_COND, SIMPLE_EVENT, RECEIVE, SEND, RECEIVE_VECTOR, SEND_VECTOR, MULTIPLE, KIND_COUNT };
static const char *const kind_names[] = {
    "delay", "thread end", "event flag", "semaphore", "mutex", "read lock", "write lock", "condition variable", "lightweight mutex",
    "lightweight condition variable", "simple event", "message pipe receive", "message pipe send", "message pipe vector receive",
    "message pipe vector send", "multiple events",
};

enum State { BLOCKS, READY, ZERO, UNKNOWN, LONG };
static const char *const state_names[] = { "blocks", "can end at once", "timeout of 0", "unknown UID" };

static SceUID holder, holder_mutex, holder_rwlock, ended, release, cb, waited_id;
static SceKernelLwMutexWork holder_lw;
static char fill[0x1000];
static volatile int calls, long_done;

static int counting_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    calls++;
    return 0;
}

// Holds a mutex, a lightweight mutex and a write lock until released
static int holder_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelLockMutex(holder_mutex, 1, NULL);
    sceKernelLockLwMutex(&holder_lw, 1, NULL);
    sceKernelLockWriteRWLock(holder_rwlock, NULL);
    sceKernelWaitSema(release, 1, NULL);
    sceKernelUnlockWriteRWLock(holder_rwlock);
    sceKernelUnlockLwMutex(&holder_lw, 1);
    sceKernelUnlockMutex(holder_mutex, 1);
    return 0;
}

static int returns_at_once(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

// Runs one wait with callbacks of the given kind on objects in the given state, and returns its
// result. A LONG wait blocks like BLOCKS, for 300 ms.
static int do_wait(int kind, int state) {
    const int ready = state == READY;
    const int unknown = state == UNKNOWN;
    SceUInt timeout = state == ZERO ? 0 : state == LONG ? 300000 : 1000;
    SceUID id = BAD_UID, other = -1;
    int result = 0;
    char buf[4] = { 0 };
    unsigned int bits;
    SceUInt32 pattern;
    SceUInt64 user_data;
    MsgPipeVector vector = { buf, sizeof(buf) };
    static SceKernelLwMutexWork lw_mutex;
    static SceKernelLwCondWork lw_cond;
    WaitEvent events[2];
    ResultEvent results[2];
    waited_id = 0;
    switch (kind) {
    case DELAY: return sceKernelDelayThreadCB(state == ZERO ? 0 : timeout);
    case THREAD_END:
        if (!unknown)
            id = ready ? ended : holder;
        waited_id = id;
        return sceKernelWaitThreadEndCB(id, NULL, &timeout);
    case EVENT_FLAG:
        if (!unknown)
            id = sceKernelCreateEventFlag("flag", 0, ready, NULL);
        waited_id = id;
        result = sceKernelWaitEventFlagCB(id, 0x1, EVF_WAITMODE_AND, &bits, &timeout);
        sceKernelDeleteEventFlag(id);
        return result;
    case SEMA:
        if (!unknown)
            id = sceKernelCreateSema("sema", 0, ready, 1, NULL);
        waited_id = id;
        result = sceKernelWaitSemaCB(id, 1, &timeout);
        sceKernelDeleteSema(id);
        return result;
    case MUTEX:
        if (!unknown)
            id = ready ? sceKernelCreateMutex("mutex", 0, 0, NULL) : holder_mutex;
        waited_id = id;
        result = sceKernelLockMutexCB(id, 1, &timeout);
        if (ready) {
            sceKernelUnlockMutex(id, 1);
            sceKernelDeleteMutex(id);
        }
        return result;
    case READ_LOCK:
    case WRITE_LOCK:
        if (!unknown)
            id = ready ? sceKernelCreateRWLock("rwlock", 0, NULL) : holder_rwlock;
        waited_id = id;
        if (kind == READ_LOCK)
            result = sceKernelLockReadRWLockCB(id, &timeout);
        else
            result = sceKernelLockWriteRWLockCB(id, &timeout);
        if (ready) {
            if (kind == READ_LOCK)
                sceKernelUnlockReadRWLock(id);
            else
                sceKernelUnlockWriteRWLock(id);
            sceKernelDeleteRWLock(id);
        }
        return result;
    case COND:
        other = sceKernelCreateMutex("cond mutex", 0, 0, NULL);
        if (!unknown)
            id = sceKernelCreateCond("cond", 0, other, NULL);
        waited_id = id;
        sceKernelLockMutex(other, 1, NULL);
        result = sceKernelWaitCondCB(id, &timeout);
        sceKernelUnlockMutex(other, 1);
        sceKernelDeleteCond(id);
        sceKernelDeleteMutex(other);
        return result;
    case LW_MUTEX:
        if (!ready)
            return sceKernelLockLwMutexCB(&holder_lw, 1, &timeout);
        sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
        result = sceKernelLockLwMutexCB(&lw_mutex, 1, &timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        sceKernelDeleteLwMutex(&lw_mutex);
        return result;
    case LW_COND:
        sceKernelCreateLwMutex(&lw_mutex, "lw cond mutex", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lw cond", 0, &lw_mutex, NULL);
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        result = sceKernelWaitLwCondCB(&lw_cond, &timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_mutex);
        return result;
    case SIMPLE_EVENT:
        if (!unknown)
            id = sceKernelCreateSimpleEvent("event", 0, ready ? EVENT_BIT_A : 0, NULL);
        waited_id = id;
        result = sceKernelWaitEventCB(id, EVENT_BIT_A, &pattern, &user_data, &timeout);
        sceKernelDeleteSimpleEvent(id);
        return result;
    case RECEIVE:
    case RECEIVE_VECTOR:
        if (!unknown) {
            id = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
            if (ready)
                sceKernelTrySendMsgPipe(id, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL);
        }
        waited_id = id;
        if (kind == RECEIVE)
            result = sceKernelReceiveMsgPipeCB(id, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout);
        else
            result = sceKernelReceiveMsgPipeVectorCB(id, &vector, 1, MSG_PIPE_MODE_FULL, NULL, &timeout);
        sceKernelDeleteMsgPipe(id);
        return result;
    case SEND:
    case SEND_VECTOR:
        if (!unknown) {
            id = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
            if (!ready)
                sceKernelTrySendMsgPipe(id, fill, sizeof(fill), MSG_PIPE_MODE_FULL, NULL);
        }
        waited_id = id;
        if (kind == SEND)
            result = sceKernelSendMsgPipeCB(id, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout);
        else
            result = sceKernelSendMsgPipeVectorCB(id, &vector, 1, MSG_PIPE_MODE_FULL, NULL, &timeout);
        sceKernelDeleteMsgPipe(id);
        return result;
    case MULTIPLE:
        if (!unknown) {
            id = sceKernelCreateSimpleEvent("first", 0, ready ? EVENT_BIT_A : 0, NULL);
            other = sceKernelCreateSimpleEvent("second", 0, 0, NULL);
        }
        events[0] = (WaitEvent){ id, EVENT_BIT_A };
        events[1] = (WaitEvent){ other, EVENT_BIT_A };
        result = sceKernelWaitMultipleEventsCB(events, unknown ? 1 : 2, EVENT_WAIT_MODE_OR, results, &timeout);
        if (!unknown) {
            sceKernelDeleteSimpleEvent(id);
            sceKernelDeleteSimpleEvent(other);
        }
        return result;
    }
    return result;
}

// Whether a kind of wait can be run in a state
static int applies(int kind, int state) {
    if (state == READY)
        return kind != DELAY && kind != COND && kind != LW_COND;
    if (state == UNKNOWN)
        return kind != DELAY && kind != LW_MUTEX && kind != LW_COND;
    return 1;
}

static int long_waiter(SceSize args, void *argp) {
    (void)args;
    do_wait(*(const int *)argp, LONG);
    long_done = 1;
    return 0;
}

// Waits until thread is in a wait, and copies its info then. Returns 0 if its wait ended first
// without the thread ever reporting one.
static int wait_snapshot(SceUID thread, SceKernelThreadInfo *info) {
    for (int i = 0; i < 5000 && !long_done; i++) {
        info->size = sizeof(*info);
        if (sceKernelGetThreadInfo(thread, info) == 0 && info->waitType != 0)
            return 1;
        sceKernelDelayThread(1000);
    }
    return 0;
}

int main(void) {
    release = sceKernelCreateSema("release", 0, 0, 1, NULL);
    holder_mutex = sceKernelCreateMutex("holder mutex", 0, 0, NULL);
    holder_rwlock = sceKernelCreateRWLock("holder rwlock", 0, NULL);
    sceKernelCreateLwMutex(&holder_lw, "holder lw mutex", 0, 0, NULL);
    holder = test_start_thread(holder_thread);
    test_wait_until_waiting(holder);
    ended = test_start_thread(returns_at_once);
    sceKernelWaitThreadEnd(ended, NULL, NULL);
    cb = sceKernelCreateCallback("pending", 0, counting_callback, NULL);

    for (int kind = 0; kind < KIND_COUNT; kind++) {
        for (int state = BLOCKS; state <= UNKNOWN; state++) {
            if (!applies(kind, state))
                continue;
            calls = 0;
            sceKernelNotifyCallback(cb, 1);
            PRINTF(do_wait(kind, state), "%s, %s: the wait", kind_names[kind], state_names[state]);
            PRINTF(calls, "%s, %s: runs of the pending callback", kind_names[kind], state_names[state]);
            sceKernelCancelCallback(cb);
        }
    }

    // Waits without callbacks and polls leave it pending
    calls = 0;
    sceKernelNotifyCallback(cb, 1);
    const SceUID sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    SceUInt timeout = 1000;
    PRINT("semaphore wait without callbacks", sceKernelWaitSema(sema, 1, &timeout));
    PRINT("poll the semaphore", sceKernelPollSema(sema, 1));
    PRINT("delay without callbacks", sceKernelDelayThread(1000));
    PRINT("runs of the pending callback after them", calls);
    sceKernelCancelCallback(cb);

    // The wait type of each kind, seen from another thread
    for (int kind = 0; kind < KIND_COUNT; kind++) {
        long_done = 0;
        const SceUID thread = test_start_thread_arg(long_waiter, kind);
        SceKernelThreadInfo info;
        const int in_wait = wait_snapshot(thread, &info);
        test_join_thread(thread);
        PRINTF(in_wait ? info.waitType : 0, "%s: the wait type", kind_names[kind]);
        if (in_wait && waited_id)
            PRINTF(info.waitId == waited_id, "%s: the wait UID is the object", kind_names[kind]);
    }

    sceKernelSignalSema(release, 1);
    test_join_thread(holder);
    test_exit();
    return 0;
}
