// The time left a wait writes back to its timeout, for the kinds of wait that timeouts,
// sema_waits and mutex_waits leave out. Each wait runs on a thread of its own, on fresh objects.
// First each wait is woken by the main thread soon after it starts waiting, with a timeout of
// 10 s, and the test prints the result and whether the time left is less than the 10 s and more
// than half of it: an event flag set, a condition variable signaled, a message pipe receive given
// a message, a message pipe send given room, a read lock and a write lock let go, a lightweight
// mutex unlocked, a lightweight condition variable signaled, a simple event set, a wait on two
// events with one of them set, a thread end and a thread signal. Then each wait that can time out
// and isn't in timeouts is left to time out after 10 ms, and the test prints the result and
// whether the time left is 0: a mutex, a read lock, a write lock, a lightweight mutex, a
// lightweight condition variable, a simple event, two events and a message pipe send.
//
// On a PS Vita TV on firmware 3.65 every kind of wait writes the time left back the same way. Each
// woken wait, the event flag, condition variable, message pipe receive and send, read and write
// lock, lightweight mutex and condition variable, simple event, two events, thread end and thread
// signal, works, the wait on two events with a result of 1, and leaves a time left under the 10 s
// timeout and over half of it. Each wait that times out fails with 0x80028005 (WAIT_TIMEOUT) and
// leaves a time left of 0.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define LONG 10000000
#define SHORT 10000

enum Kind {
    EVENT_FLAG,
    COND,
    RECEIVE,
    SEND,
    READ_LOCK,
    WRITE_LOCK,
    LW_MUTEX,
    LW_COND,
    SIMPLE_EVENT,
    MULTIPLE,
    THREAD_END,
    SIGNAL,
    MUTEX,
    KIND_COUNT
};

static const char *const kind_names[] = {
    "event flag", "condition variable", "message pipe receive", "message pipe send", "read lock",
    "write lock", "lightweight mutex", "lightweight condition variable", "simple event",
    "two events", "thread end", "thread signal", "mutex",
};

static volatile int kind, wait_result;
static volatile SceUInt left;
static SceUInt given;
static SceUID object, other, target, release;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
// A Vita rounds a pipe's buffer up to 4 KiB, so the pipe is created with that size
static char fill[0x1000];

// Whether the thread is in a wait
static int waiting(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType != 0;
}

static int target_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelWaitSema(release, 1, NULL);
    return 0;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = given;
    unsigned int bits;
    SceUInt32 pattern;
    SceUInt64 user_data;
    char buf[4] = { 0 };
    WaitEvent events[2] = { { object, EVENT_BIT_A }, { other, EVENT_BIT_A } };
    ResultEvent results[2];
    int result = -1;
    switch (kind) {
    case EVENT_FLAG:
        result = sceKernelWaitEventFlag(object, 1, SCE_EVENT_WAITAND, &bits, &timeout);
        break;
    case COND:
        sceKernelLockMutex(other, 1, NULL);
        result = sceKernelWaitCond(object, &timeout);
        sceKernelUnlockMutex(other, 1);
        break;
    case RECEIVE:
        result = sceKernelReceiveMsgPipe(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL,
            &timeout);
        break;
    case SEND:
        result = sceKernelSendMsgPipe(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL,
            &timeout);
        break;
    case READ_LOCK:
        result = sceKernelLockReadRWLock(object, &timeout);
        if (result == 0)
            sceKernelUnlockReadRWLock(object);
        break;
    case WRITE_LOCK:
        result = sceKernelLockWriteRWLock(object, &timeout);
        if (result == 0)
            sceKernelUnlockWriteRWLock(object);
        break;
    case LW_MUTEX:
        result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        if (result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        result = sceKernelWaitLwCond(&lw_cond, &timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case SIMPLE_EVENT:
        result = sceKernelWaitEvent(object, EVENT_BIT_A, &pattern, &user_data, &timeout);
        break;
    case MULTIPLE:
        result = sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, results, &timeout);
        break;
    case THREAD_END: result = sceKernelWaitThreadEnd(target, NULL, &timeout); break;
    case SIGNAL: result = sceKernelWaitSignal(0, 0, &timeout); break;
    case MUTEX:
        result = sceKernelLockMutex(object, 1, &timeout);
        if (result == 0)
            sceKernelUnlockMutex(object, 1);
        break;
    }
    wait_result = result;
    left = timeout;
    return 0;
}

// Makes the objects for the wait, held or full so that it blocks
static void make(void) {
    switch (kind) {
    case EVENT_FLAG: object = sceKernelCreateEventFlag("left", 0, 0, NULL); break;
    case COND:
        other = sceKernelCreateMutex("left cond", 0, 0, NULL);
        object = sceKernelCreateCond("left", 0, other, NULL);
        break;
    case RECEIVE:
        object = sceKernelCreateMsgPipe("left", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
        break;
    case SEND:
        object = sceKernelCreateMsgPipe("left", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
        sceKernelTrySendMsgPipe(object, fill, sizeof(fill), MSG_PIPE_MODE_FULL, NULL);
        break;
    case READ_LOCK:
        object = sceKernelCreateRWLock("left", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case WRITE_LOCK:
        object = sceKernelCreateRWLock("left", 0, NULL);
        sceKernelLockReadRWLock(object, NULL);
        break;
    case LW_MUTEX:
        sceKernelCreateLwMutex(&lw_mutex, "left", 0, 1, NULL);
        break;
    case LW_COND:
        sceKernelCreateLwMutex(&lw_mutex, "left cond", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "left", 0, &lw_mutex, NULL);
        break;
    case SIMPLE_EVENT: object = sceKernelCreateSimpleEvent("left", 0, 0, NULL); break;
    case MULTIPLE:
        object = sceKernelCreateSimpleEvent("left", 0, 0, NULL);
        other = sceKernelCreateSimpleEvent("left other", 0, 0, NULL);
        break;
    case THREAD_END:
        release = sceKernelCreateSema("left release", 0, 0, 1, NULL);
        target = test_start_thread(target_thread);
        TEST_WAIT_FOR(waiting(target));
        break;
    case SIGNAL: break;
    case MUTEX:
        object = sceKernelCreateMutex("left", 0, 1, NULL);
        break;
    }
}

// Ends the wait of the waiter as its object allows
static void wake(SceUID thread) {
    switch (kind) {
    case EVENT_FLAG: sceKernelSetEventFlag(object, 1); break;
    case COND: sceKernelSignalCond(object); break;
    case RECEIVE: sceKernelTrySendMsgPipe(object, fill, 4, MSG_PIPE_MODE_FULL, NULL); break;
    case SEND: sceKernelTryReceiveMsgPipe(object, fill, 4, MSG_PIPE_MODE_FULL, NULL); break;
    case READ_LOCK: sceKernelUnlockWriteRWLock(object); break;
    case WRITE_LOCK: sceKernelUnlockReadRWLock(object); break;
    case LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        sceKernelSignalLwCond(&lw_cond);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case SIMPLE_EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    case MULTIPLE: sceKernelSetEvent(other, EVENT_BIT_A, 0); break;
    case THREAD_END: sceKernelSignalSema(release, 1); break;
    case SIGNAL: sceKernelSendSignal(thread); break;
    case MUTEX: sceKernelUnlockMutex(object, 1); break;
    }
}

// Lets go of what the main thread holds after a wait that timed out, and deletes the objects
static void destroy(int woken) {
    switch (kind) {
    case EVENT_FLAG: sceKernelDeleteEventFlag(object); break;
    case COND:
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(other);
        break;
    case RECEIVE:
    case SEND: sceKernelDeleteMsgPipe(object); break;
    case READ_LOCK:
        if (!woken)
            sceKernelUnlockWriteRWLock(object);
        sceKernelDeleteRWLock(object);
        break;
    case WRITE_LOCK:
        if (!woken)
            sceKernelUnlockReadRWLock(object);
        sceKernelDeleteRWLock(object);
        break;
    case LW_MUTEX:
        if (!woken)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        sceKernelDeleteLwMutex(&lw_mutex);
        break;
    case LW_COND:
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_mutex);
        break;
    case SIMPLE_EVENT: sceKernelDeleteSimpleEvent(object); break;
    case MULTIPLE:
        sceKernelDeleteSimpleEvent(object);
        sceKernelDeleteSimpleEvent(other);
        break;
    case THREAD_END:
        if (!woken)
            sceKernelSignalSema(release, 1);
        test_join_thread(target);
        sceKernelDeleteSema(release);
        break;
    case SIGNAL: break;
    case MUTEX:
        if (!woken)
            sceKernelUnlockMutex(object, 1);
        sceKernelDeleteMutex(object);
        break;
    }
}

static void woken(int which) {
    kind = which;
    given = LONG;
    make();
    const SceUID thread = test_start_thread(waiter);
    TEST_WAIT_FOR(waiting(thread));
    wake(thread);
    test_join_thread(thread);
    destroy(1);
    const char *name = kind_names[which];
    PRINTF(wait_result, "%s, woken: the wait", name);
    PRINTF(left < LONG, "%s, woken: the time left is less than the timeout", name);
    PRINTF(left > LONG / 2, "%s, woken: the time left is more than half of it", name);
}

static void timed_out(int which) {
    kind = which;
    given = SHORT;
    make();
    const SceUID thread = test_start_thread(waiter);
    test_join_thread(thread);
    destroy(0);
    const char *name = kind_names[which];
    PRINTF(wait_result, "%s, timed out: the wait", name);
    PRINTF(left == 0, "%s, timed out: the time left is 0", name);
}

int main(void) {
    for (int which = 0; which < MUTEX; which++)
        woken(which);
    static const int timing_out[] = {
        MUTEX, READ_LOCK, WRITE_LOCK, LW_MUTEX, LW_COND, SIMPLE_EVENT, MULTIPLE, SEND,
    };
    for (unsigned int i = 0; i < sizeof(timing_out) / sizeof(timing_out[0]); i++)
        timed_out(timing_out[i]);
    test_exit();
    return 0;
}
