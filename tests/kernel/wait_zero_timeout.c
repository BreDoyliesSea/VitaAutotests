// Every blocking wait with a timeout of 0, when it can't end at once and when it can: whether it
// fails at once or still ends, and the time it writes back
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { THREAD_END, EVENT_FLAG, SEMA, MUTEX, READ_LOCK, WRITE_LOCK, COND, LW_MUTEX, SIMPLE_EVENT, RECEIVE, SEND, MULTIPLE, KIND_COUNT };
static const char *const kind_names[] = {
    "thread end", "event flag", "semaphore", "mutex", "read lock", "write lock", "condition variable", "lightweight mutex",
    "simple event", "message pipe receive", "message pipe send", "multiple events",
};

static SceUID holder, holder_mutex, holder_rwlock, ended, release;
static SceKernelLwMutexWork holder_lw;
static char fill[0x1000];

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

// Runs one wait with a timeout of 0, on objects that let it end at once if ready, and stores the
// time it wrote back
static int do_wait(int kind, int ready, SceUInt *timeout) {
    *timeout = 0;
    SceUID id, other;
    int result = 0;
    char buf[4] = { 0 };
    unsigned int bits;
    SceUInt32 pattern;
    SceUInt64 user_data;
    static SceKernelLwMutexWork lw_mutex;
    WaitEvent events[2];
    ResultEvent results[2];
    switch (kind) {
    case THREAD_END: return sceKernelWaitThreadEnd(ready ? ended : holder, NULL, timeout);
    case EVENT_FLAG:
        id = sceKernelCreateEventFlag("flag", 0, ready, NULL);
        result = sceKernelWaitEventFlag(id, 0x1, EVF_WAITMODE_AND, &bits, timeout);
        sceKernelDeleteEventFlag(id);
        return result;
    case SEMA:
        id = sceKernelCreateSema("sema", 0, ready, 1, NULL);
        result = sceKernelWaitSema(id, 1, timeout);
        sceKernelDeleteSema(id);
        return result;
    case MUTEX:
        id = ready ? sceKernelCreateMutex("mutex", 0, 0, NULL) : holder_mutex;
        result = sceKernelLockMutex(id, 1, timeout);
        if (ready) {
            sceKernelUnlockMutex(id, 1);
            sceKernelDeleteMutex(id);
        }
        return result;
    case READ_LOCK:
    case WRITE_LOCK:
        id = ready ? sceKernelCreateRWLock("rwlock", 0, NULL) : holder_rwlock;
        result = kind == READ_LOCK ? sceKernelLockReadRWLock(id, timeout) : sceKernelLockWriteRWLock(id, timeout);
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
        id = sceKernelCreateCond("cond", 0, other, NULL);
        sceKernelLockMutex(other, 1, NULL);
        result = sceKernelWaitCond(id, timeout);
        sceKernelUnlockMutex(other, 1);
        sceKernelDeleteCond(id);
        sceKernelDeleteMutex(other);
        return result;
    case LW_MUTEX:
        if (!ready)
            return sceKernelLockLwMutex(&holder_lw, 1, timeout);
        sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
        result = sceKernelLockLwMutex(&lw_mutex, 1, timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        sceKernelDeleteLwMutex(&lw_mutex);
        return result;
    case SIMPLE_EVENT:
        id = sceKernelCreateSimpleEvent("event", 0, ready ? EVENT_BIT_A : 0, NULL);
        result = sceKernelWaitEvent(id, EVENT_BIT_A, &pattern, &user_data, timeout);
        sceKernelDeleteSimpleEvent(id);
        return result;
    case RECEIVE:
        id = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
        if (ready)
            sceKernelTrySendMsgPipe(id, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL);
        result = sceKernelReceiveMsgPipe(id, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, timeout);
        sceKernelDeleteMsgPipe(id);
        return result;
    case SEND:
        id = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
        if (!ready)
            sceKernelTrySendMsgPipe(id, fill, sizeof(fill), MSG_PIPE_MODE_FULL, NULL);
        result = sceKernelSendMsgPipe(id, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, timeout);
        sceKernelDeleteMsgPipe(id);
        return result;
    case MULTIPLE:
        id = sceKernelCreateSimpleEvent("first", 0, ready ? EVENT_BIT_A : 0, NULL);
        other = sceKernelCreateSimpleEvent("second", 0, 0, NULL);
        events[0] = (WaitEvent){ id, EVENT_BIT_A };
        events[1] = (WaitEvent){ other, EVENT_BIT_A };
        result = sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, results, timeout);
        sceKernelDeleteSimpleEvent(id);
        sceKernelDeleteSimpleEvent(other);
        return result;
    }
    return result;
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

    for (int kind = 0; kind < KIND_COUNT; kind++) {
        for (int ready = 0; ready <= 1; ready++) {
            if (ready && kind == COND)
                continue;
            SceUInt timeout = 0xDEAD;
            const char *state = ready ? "can end at once" : "can't end at once";
            PRINTF(do_wait(kind, ready, &timeout), "%s, %s: the wait", kind_names[kind], state);
            PRINTF(timeout, "%s, %s: the time it wrote back", kind_names[kind], state);
        }
    }

    sceKernelSignalSema(release, 1);
    test_join_thread(holder);
    test_exit();
    return 0;
}
