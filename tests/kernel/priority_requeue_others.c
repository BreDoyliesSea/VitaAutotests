// Where a waiting thread goes in a priority ordered queue when its priority is raised while it
// waits, for each kind of object with such a queue. Two low priority waiters, then one at a higher
// priority, then the second low one raised to that priority. The waiters wake one at a time, or
// hand a lock on to each other, and record the order they got it in.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define LOW_PRIORITY 0x10000102
#define MID_PRIORITY 0x10000100

enum Kind { MUTEX, EVENT_FLAG, SIMPLE_EVENT, MSG_PIPE, WRITE_LOCK, LW_MUTEX, COND, COND_MUTEX, KIND_COUNT };
static const char *const kind_names[] = {
    "mutex", "event flag", "simple event", "message pipe", "write lock", "lightweight mutex", "condition variable",
    "condition variable, waiting for its mutex",
};

static SceUID object, mutex;
static SceKernelLwMutexWork lw_mutex;
static volatile int kind, order[3], woken;

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    unsigned int bits;
    SceUInt32 pattern;
    SceUInt64 user_data;
    char c;
    switch (kind) {
    case MUTEX:
        if (sceKernelLockMutex(object, 1, &timeout) == 0) {
            order[i] = ++woken;
            sceKernelUnlockMutex(object, 1);
        }
        break;
    case EVENT_FLAG:
        if (sceKernelWaitEventFlag(object, 0x1, EVF_WAITMODE_AND | EVF_WAITMODE_CLEAR_ALL, &bits, &timeout) == 0)
            order[i] = ++woken;
        break;
    case SIMPLE_EVENT:
        if (sceKernelWaitEvent(object, EVENT_BIT_A, &pattern, &user_data, &timeout) == 0)
            order[i] = ++woken;
        break;
    case MSG_PIPE:
        if (sceKernelReceiveMsgPipe(object, &c, 1, MSG_PIPE_MODE_FULL, NULL, &timeout) == 0)
            order[i] = ++woken;
        break;
    case WRITE_LOCK:
        if (sceKernelLockWriteRWLock(object, &timeout) == 0) {
            order[i] = ++woken;
            sceKernelUnlockWriteRWLock(object);
        }
        break;
    case LW_MUTEX:
        if (sceKernelLockLwMutex(&lw_mutex, 1, &timeout) == 0) {
            order[i] = ++woken;
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        }
        break;
    case COND:
    case COND_MUTEX:
        sceKernelLockMutex(mutex, 1, NULL);
        if (sceKernelWaitCond(object, &timeout) == 0)
            order[i] = ++woken;
        sceKernelUnlockMutex(mutex, 1);
        break;
    }
    return 0;
}

static SceUID start(int i, int priority) {
    const SceUID thread = sceKernelCreateThread("waiter", waiter, priority, 0x4000, 0, 0, NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    test_wait_until_waiting(thread);
    return thread;
}

static int in_mutex_wait(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType != 0 && info.waitType != 0x100;
}

static void run(int new_kind) {
    kind = new_kind;
    woken = order[0] = order[1] = order[2] = 0;
    const char *name = kind_names[new_kind];
    int created = 0;
    switch (new_kind) {
    case MUTEX: created = object = sceKernelCreateMutex("mutex", ATTR_TH_PRIO, 1, NULL); break;
    case EVENT_FLAG: created = object = sceKernelCreateEventFlag("flag", ATTR_TH_PRIO | EVF_ATTR_MULTI, 0, NULL); break;
    case SIMPLE_EVENT: created = object = sceKernelCreateSimpleEvent("event", ATTR_TH_PRIO | EVENT_ATTR_AUTO_RESET, 0, NULL); break;
    // Attribute 0x2 is the one that orders a message pipe's receivers by priority (queue_order)
    case MSG_PIPE: created = object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0x2, 64, NULL); break;
    case WRITE_LOCK:
        created = object = sceKernelCreateRWLock("rwlock", ATTR_TH_PRIO, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case LW_MUTEX: created = sceKernelCreateLwMutex(&lw_mutex, "lw mutex", ATTR_TH_PRIO, 1, NULL); break;
    case COND:
        mutex = sceKernelCreateMutex("cond mutex", 0, 0, NULL);
        created = object = sceKernelCreateCond("cond", ATTR_TH_PRIO, mutex, NULL);
        break;
    case COND_MUTEX:
        mutex = sceKernelCreateMutex("cond mutex", ATTR_TH_PRIO, 0, NULL);
        created = object = sceKernelCreateCond("cond", ATTR_TH_PRIO, mutex, NULL);
        break;
    }
    PRINTF(test_created(created), "%s: create it with the priority attribute", name);
    if (created < 0)
        return;

    SceUID threads[3];
    threads[0] = start(0, LOW_PRIORITY);
    threads[1] = start(1, LOW_PRIORITY);
    threads[2] = start(2, MID_PRIORITY);
    if (new_kind == COND_MUTEX) {
        // Moves the waiters from the condition to its mutex, which this thread holds, one at a time
        // so they queue for the mutex in the order they waited
        sceKernelLockMutex(mutex, 1, NULL);
        for (int i = 0; i < 3; i++) {
            sceKernelSignalCondTo(object, threads[i]);
            TEST_WAIT_FOR(in_mutex_wait(threads[i]));
        }
    }
    sceKernelChangeThreadPriority(threads[1], MID_PRIORITY);

    switch (new_kind) {
    case MUTEX: sceKernelUnlockMutex(object, 1); break;
    case WRITE_LOCK: sceKernelUnlockWriteRWLock(object); break;
    case LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case COND_MUTEX: sceKernelUnlockMutex(mutex, 1); break;
    default:
        for (int i = 0; i < 3; i++) {
            char c = 'x';
            switch (new_kind) {
            case EVENT_FLAG: sceKernelSetEventFlag(object, 0x1); break;
            case SIMPLE_EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
            case MSG_PIPE: sceKernelSendMsgPipe(object, &c, 1, MSG_PIPE_MODE_FULL, NULL, NULL); break;
            case COND: sceKernelSignalCond(object); break;
            }
            TEST_WAIT_FOR(woken == i + 1);
        }
        break;
    }
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);
    PRINTF(order[0], "%s: the low priority waiter that waited first", name);
    PRINTF(order[1], "%s: the raised waiter", name);
    PRINTF(order[2], "%s: the waiter already at that priority", name);

    switch (new_kind) {
    case MUTEX: sceKernelDeleteMutex(object); break;
    case EVENT_FLAG: sceKernelDeleteEventFlag(object); break;
    case SIMPLE_EVENT: sceKernelDeleteSimpleEvent(object); break;
    case MSG_PIPE: sceKernelDeleteMsgPipe(object); break;
    case WRITE_LOCK: sceKernelDeleteRWLock(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    default:
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(mutex);
        break;
    }
}

int main(void) {
    for (int kind = 0; kind < KIND_COUNT; kind++)
        run(kind);

    test_exit();
    return 0;
}
