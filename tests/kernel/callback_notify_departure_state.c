// What a waited object and its waiting thread T show right after sceKernelNotifyCallback of T's
// callback while T waits with callbacks, before T can run, and again once T ran the callback and
// waits again. Every thread is pinned to core 0. The main thread runs at 0x60 and T at 0xB0, so T
// stays ready until the main thread blocks. T creates its callback in its own thread before it
// waits, and the callback counts its runs. Every wait has a timeout of a few seconds, and every
// case has a fresh object and a fresh T. The main thread waits until T waits, notifies the
// callback, and right away, with no call that can block in between, reads the object's info and T's
// thread info. Then it blocks until the callback ran and T waits again, and reads both again. After
// that it ends T's wait, joins T, and prints. The prints come last, since a print blocks the main
// thread and lets T run. Cases:
// - a semaphore at 0, T in sceKernelWaitSemaCB
// - an event flag with SCE_EVENT_WAITMULTIPLE, T in sceKernelWaitEventFlagCB for a bit not set
// - an event flag without it, the same wait
// - a mutex that the main thread holds, T in sceKernelLockMutexCB
// - a lightweight mutex that the main thread holds, T in sceKernelLockLwMutexCB
// - a rwlock that the main thread holds for writing, T in sceKernelLockWriteRWLockCB
// - the same rwlock, T in sceKernelLockReadRWLockCB
// - a simple event, T in sceKernelWaitEventCB
// - an empty message pipe, T in sceKernelReceiveMsgPipeCB
// - a full message pipe, T in sceKernelSendMsgPipeCB
// - a condition, T locks its mutex and waits in sceKernelWaitCondCB
// - a lightweight condition, T locks its mutex and waits in sceKernelWaitLwCondCB
// - a semaphore at 0, T in sceKernelWaitSema, a wait without callbacks, as a reference
// The calls for the object's info are sceKernelGetSemaInfo, sceKernelGetEventFlagInfo,
// sceKernelGetMutexInfo, sceKernelGetLwMutexInfo, sceKernelGetRWLockInfo, sceKernelGetEventInfo,
// sceKernelGetMsgPipeInfo, sceKernelGetCondInfo and sceKernelGetLwCondInfo.
//
// Each case prints:
// - whether T waits on the object with the wait type of its kind before the notify, and the
//   object's counts of waiting threads then. A rwlock has one count for readers and one for
//   writers, and a message pipe one for senders and one for receivers. Every other object has one.
// - the result of the notify
// - after the notify: the object's counts of waiting threads, T's status (2 is ready, 8 is
//   waiting), T's wait type (0 is none, bit 0x80000000 marks a wait with callbacks), and whether
//   T's wait UID is the object
// - how often the callback ran, then the same four values once T waits again
// - the result of the call that ends T's wait, and T's wait result
// The reference case, whose wait ignores the notify, prints the four values right after the notify
// only. T runs sceKernelCheckCallback once its wait ended, and the case prints how often the
// callback ran before that call and after it.
//
// On a PS Vita TV on firmware 3.65, for every wait with callbacks, the object counts no waiting
// thread right after the notify, and T is ready (0x2) with wait type 0 and a wait UID that isn't
// the object, before it runs. Once its callback ran, T waits again on the object, with its wait
// type and the callback bit, and the object counts it again. The wait without callbacks ignores the
// notify: the object still counts T, which still waits, and the callback runs only at T's check.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
#define LONG_TIMEOUT (3 * TEST_WAIT_US)
#define PIPE_SIZE 4096

typedef enum Kind {
    SEMA,
    FLAG_MULTIPLE,
    FLAG_SINGLE,
    MUTEX,
    LW_MUTEX,
    RW_WRITE,
    RW_READ,
    SIMPLE_EVENT,
    PIPE_RECEIVE,
    PIPE_SEND,
    COND,
    LW_COND,
    SEMA_PLAIN
} Kind;

typedef struct Case {
    const char *name;
    Kind kind;
    // The wait type of T's wait without WAIT_TYPE_CB
    SceUInt32 wait_type;
    // What the object's counts of waiting threads are called, second is NULL if the object has one
    const char *first, *second;
} Case;

// What the main thread reads while T is ready or waiting
typedef struct State {
    int first, second;
    int status;
    SceUInt32 wait_type;
    int on_object;
} State;

static SceUID object, cond_mutex, callback;
static SceKernelLwMutexWork lw_mutex, lw_cond_mutex;
static SceKernelLwCondWork lw_cond;
static Kind current;
static char t_buf[4], main_buf[4] = { 'a', 'b', 'c', 'd' }, fill[PIPE_SIZE];
static volatile int result, callback_runs, runs_before_check, runs_after_check;

static int notified_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    return 0;
}

// Creates its callback, then waits on the object of the current case
static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = LONG_TIMEOUT;
    SceUInt32 bits = 0, pattern = 0;
    SceUInt64 user_data = 0;
    callback = sceKernelCreateCallback("notified", 0, notified_callback, NULL);
    switch (current) {
    case SEMA: result = sceKernelWaitSemaCB(object, 1, &timeout); break;
    case FLAG_MULTIPLE:
    case FLAG_SINGLE: result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case MUTEX:
        result = sceKernelLockMutexCB(object, 1, &timeout);
        if (result == 0)
            sceKernelUnlockMutex(object, 1);
        break;
    case LW_MUTEX:
        result = sceKernelLockLwMutexCB(&lw_mutex, 1, &timeout);
        if (result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case RW_WRITE:
        result = sceKernelLockWriteRWLockCB(object, &timeout);
        if (result == 0)
            sceKernelUnlockWriteRWLock(object);
        break;
    case RW_READ:
        result = sceKernelLockReadRWLockCB(object, &timeout);
        if (result == 0)
            sceKernelUnlockReadRWLock(object);
        break;
    case SIMPLE_EVENT: result = sceKernelWaitEventCB(object, EVENT_BIT_A, &pattern, &user_data, &timeout); break;
    case PIPE_RECEIVE: result = sceKernelReceiveMsgPipeCB(object, t_buf, sizeof(t_buf), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    case PIPE_SEND: result = sceKernelSendMsgPipeCB(object, t_buf, sizeof(t_buf), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    case COND:
        sceKernelLockMutex(cond_mutex, 1, NULL);
        result = sceKernelWaitCondCB(object, &timeout);
        sceKernelUnlockMutex(cond_mutex, 1);
        break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_cond_mutex, 1, NULL);
        result = sceKernelWaitLwCondCB(&lw_cond, &timeout);
        sceKernelUnlockLwMutex(&lw_cond_mutex, 1);
        break;
    case SEMA_PLAIN:
        result = sceKernelWaitSema(object, 1, &timeout);
        runs_before_check = callback_runs;
        sceKernelCheckCallback();
        runs_after_check = callback_runs;
        break;
    }
    sceKernelDeleteCallback(callback);
    return 0;
}

// Returns the UID that T's wait on the lightweight mutex reports
static SceUID lw_mutex_uid(SceKernelLwMutexWork *work) {
    LwMutexInfo info = { .size = sizeof(info) };
    return sceKernelGetLwMutexInfo(work, &info) == 0 ? info.uid : -1;
}

// Returns the UID that T's wait on the lightweight condition reports
static SceUID lw_cond_uid(SceKernelLwCondWork *work) {
    LwCondInfo info = { .size = sizeof(info) };
    return sceKernelGetLwCondInfo(work, &info) == 0 ? info.uid : -1;
}

// Creates the object of a case, and puts it in the state T waits on. Sets object to the UID that
// T's wait reports.
static void create_object(Kind kind) {
    SceUInt timeout = LONG_TIMEOUT;
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case FLAG_MULTIPLE: object = sceKernelCreateEventFlag("flag", SCE_EVENT_WAITMULTIPLE, 0, NULL); break;
    case FLAG_SINGLE: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case MUTEX:
        object = sceKernelCreateMutex("mutex", 0, 0, NULL);
        sceKernelLockMutex(object, 1, NULL);
        break;
    case LW_MUTEX:
        sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        object = lw_mutex_uid(&lw_mutex);
        break;
    case RW_WRITE:
    case RW_READ:
        object = sceKernelCreateRWLock("rwlock", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case SIMPLE_EVENT: object = sceKernelCreateSimpleEvent("event", 0, 0, NULL); break;
    case PIPE_RECEIVE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, PIPE_SIZE, NULL); break;
    case PIPE_SEND:
        object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, PIPE_SIZE, NULL);
        sceKernelSendMsgPipe(object, fill, sizeof(fill), MSG_PIPE_MODE_FULL, NULL, &timeout);
        break;
    case COND:
        cond_mutex = sceKernelCreateMutex("cond mutex", 0, 0, NULL);
        object = sceKernelCreateCond("cond", 0, cond_mutex, NULL);
        break;
    case LW_COND:
        sceKernelCreateLwMutex(&lw_cond_mutex, "lw cond mutex", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lw cond", 0, &lw_cond_mutex, NULL);
        object = lw_cond_uid(&lw_cond);
        break;
    }
}

// Ends T's wait and returns the result of the call that does it
static int end_wait(Kind kind) {
    SceUInt timeout = LONG_TIMEOUT;
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: return sceKernelSignalSema(object, 1);
    case FLAG_MULTIPLE:
    case FLAG_SINGLE: return sceKernelSetEventFlag(object, 0x1);
    case MUTEX: return sceKernelUnlockMutex(object, 1);
    case LW_MUTEX: return sceKernelUnlockLwMutex(&lw_mutex, 1);
    case RW_WRITE:
    case RW_READ: return sceKernelUnlockWriteRWLock(object);
    case SIMPLE_EVENT: return sceKernelSetEvent(object, EVENT_BIT_A, 0);
    case PIPE_RECEIVE: return sceKernelSendMsgPipe(object, main_buf, sizeof(main_buf), MSG_PIPE_MODE_FULL, NULL, &timeout);
    case PIPE_SEND: return sceKernelReceiveMsgPipe(object, main_buf, sizeof(main_buf), MSG_PIPE_MODE_FULL, NULL, &timeout);
    case COND: return sceKernelSignalCond(object);
    case LW_COND: return sceKernelSignalLwCond(&lw_cond);
    }
    return NOT_DONE;
}

static void delete_object(Kind kind) {
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: sceKernelDeleteSema(object); break;
    case FLAG_MULTIPLE:
    case FLAG_SINGLE: sceKernelDeleteEventFlag(object); break;
    case MUTEX: sceKernelDeleteMutex(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case RW_WRITE:
    case RW_READ: sceKernelDeleteRWLock(object); break;
    case SIMPLE_EVENT: sceKernelDeleteSimpleEvent(object); break;
    case PIPE_RECEIVE:
    case PIPE_SEND: sceKernelDeleteMsgPipe(object); break;
    case COND:
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(cond_mutex);
        break;
    case LW_COND:
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_cond_mutex);
        break;
    }
}

// Reads the counts of waiting threads of the object of a case, which stay NOT_DONE if its info
// call fails
static void read_counts(Kind kind, int *first, int *second) {
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: {
        SceKernelSemaInfo info = { .size = sizeof(info) };
        if (sceKernelGetSemaInfo(object, &info) == 0)
            *first = info.numWaitThreads;
        break;
    }
    case FLAG_MULTIPLE:
    case FLAG_SINGLE: {
        SceKernelEventFlagInfo info = { .size = sizeof(info) };
        if (sceKernelGetEventFlagInfo(object, &info) == 0)
            *first = info.numWaitThreads;
        break;
    }
    case MUTEX: {
        MutexInfo info = { .size = sizeof(info) };
        if (sceKernelGetMutexInfo(object, (SceKernelMutexInfo *)&info) == 0)
            *first = info.numWaitThreads;
        break;
    }
    case LW_MUTEX: {
        LwMutexInfo info = { .size = sizeof(info) };
        if (sceKernelGetLwMutexInfo(&lw_mutex, &info) == 0)
            *first = info.numWaitThreads;
        break;
    }
    case RW_WRITE:
    case RW_READ: {
        SceKernelRWLockInfo info = { .size = sizeof(info) };
        if (sceKernelGetRWLockInfo(object, &info) == 0) {
            *first = info.numReadWaitThreads;
            *second = info.numWriteWaitThreads;
        }
        break;
    }
    case SIMPLE_EVENT: {
        EventInfo info = { .size = sizeof(info) };
        if (sceKernelGetEventInfo(object, &info) == 0)
            *first = info.numWaitThreads;
        break;
    }
    case PIPE_RECEIVE:
    case PIPE_SEND: {
        SceKernelMppInfo info = { .size = sizeof(info) };
        if (sceKernelGetMsgPipeInfo(object, &info) == 0) {
            *first = info.numSendWaitThreads;
            *second = info.numReceiveWaitThreads;
        }
        break;
    }
    case COND: {
        SceKernelCondInfo info = { .size = sizeof(info) };
        if (sceKernelGetCondInfo(object, &info) == 0)
            *first = info.numWaitThreads;
        break;
    }
    case LW_COND: {
        LwCondInfo info = { .size = sizeof(info) };
        if (sceKernelGetLwCondInfo(&lw_cond, &info) == 0)
            *first = info.numWaitThreads;
        break;
    }
    }
}

// Reads the object's info, then T's thread info, with no call that can block
static State read_state(Kind kind, SceUID thread) {
    State s = { .first = NOT_DONE, .second = NOT_DONE, .status = NOT_DONE, .wait_type = NOT_DONE };
    read_counts(kind, &s.first, &s.second);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) == 0) {
        s.status = info.status;
        s.wait_type = info.waitType;
        s.on_object = info.waitId == object;
    }
    return s;
}

static void print_counts(const Case *c, const char *when, const State *s) {
    PRINTF(s->first, "%s: the %s %s", c->name, c->first, when);
    if (c->second)
        PRINTF(s->second, "%s: the %s %s", c->name, c->second, when);
}

static void print_state(const Case *c, const char *when, const State *s) {
    print_counts(c, when, s);
    PRINTF(s->status, "%s: T's status %s", c->name, when);
    PRINTF(s->wait_type, "%s: T's wait type %s", c->name, when);
    PRINTF(s->on_object, "%s: T's wait UID is the object %s", c->name, when);
}

static void run_case(const Case *c) {
    const int with_callbacks = c->kind != SEMA_PLAIN;
    const SceUInt32 type = c->wait_type | (with_callbacks ? WAIT_TYPE_CB : 0);
    current = c->kind;
    result = NOT_DONE;
    callback = -1;
    callback_runs = 0;
    runs_before_check = runs_after_check = NOT_DONE;
    create_object(c->kind);
    const SceUID thread =
        sceKernelCreateThread("t", waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, type, object));
    const int waits = test_in_wait(thread, type, object);
    const State before = read_state(c->kind, thread);

    const int notify = sceKernelNotifyCallback(callback, 1);
    const State after = read_state(c->kind, thread);

    int runs = NOT_DONE;
    State again = { .first = NOT_DONE, .second = NOT_DONE, .status = NOT_DONE, .wait_type = NOT_DONE };
    if (with_callbacks) {
        TEST_WAIT_FOR(callback_runs > 0 && test_in_wait(thread, type, object));
        runs = callback_runs;
        again = read_state(c->kind, thread);
    }
    const int end = end_wait(c->kind);
    test_join_thread(thread);

    PRINTF(waits, "%s: T waits before the notify", c->name);
    print_counts(c, "before the notify", &before);
    PRINTF(notify, "%s: the notify", c->name);
    print_state(c, "after the notify", &after);
    if (with_callbacks) {
        PRINTF(runs, "%s: runs of the callback once T waits again", c->name);
        print_state(c, "after T waits again", &again);
    }
    PRINTF(end, "%s: the call that ends T's wait", c->name);
    PRINTF(result, "%s: T's wait result", c->name);
    if (!with_callbacks) {
        PRINTF(runs_before_check, "%s: runs of the callback before T's check", c->name);
        PRINTF(runs_after_check, "%s: runs of the callback after T's check", c->name);
    }
    delete_object(c->kind);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    static const Case cases[] = {
        { "semaphore", SEMA, WAIT_TYPE_SEMA, "waiting threads", NULL },
        { "event flag with multiple waiters", FLAG_MULTIPLE, WAIT_TYPE_EVENT_FLAG, "waiting threads", NULL },
        { "event flag with one waiter", FLAG_SINGLE, WAIT_TYPE_EVENT_FLAG, "waiting threads", NULL },
        { "mutex", MUTEX, WAIT_TYPE_MUTEX, "waiting threads", NULL },
        { "lightweight mutex", LW_MUTEX, WAIT_TYPE_LW_MUTEX, "waiting threads", NULL },
        { "rwlock write waiter", RW_WRITE, WAIT_TYPE_RW_LOCK, "waiting readers", "waiting writers" },
        { "rwlock read waiter", RW_READ, WAIT_TYPE_RW_LOCK, "waiting readers", "waiting writers" },
        { "simple event", SIMPLE_EVENT, WAIT_TYPE_EVENT, "waiting threads", NULL },
        { "message pipe receive", PIPE_RECEIVE, WAIT_TYPE_MSG_PIPE, "waiting senders", "waiting receivers" },
        { "message pipe send", PIPE_SEND, WAIT_TYPE_MSG_PIPE, "waiting senders", "waiting receivers" },
        { "condition", COND, WAIT_TYPE_COND, "waiting threads", NULL },
        { "lightweight condition", LW_COND, WAIT_TYPE_LW_COND, "waiting threads", NULL },
        { "semaphore without callbacks", SEMA_PLAIN, WAIT_TYPE_SEMA, "waiting threads", NULL },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);

    test_exit();
    return 0;
}
