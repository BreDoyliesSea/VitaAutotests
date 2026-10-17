// A waiter on an openable object of each kind with an open call when the object loses its last UID.
// Each case makes the object, opens it once by its name, and starts a thread that waits on it.
// Then the main thread deletes the creating UID, reads right after how many waiters the info call
// through the opened UID counts, and closes the opened UID, the object's last. Then it waits for
// the thread's end, and the test prints the delete, the waiters it counted, the close, what the
// wait returned and whether the wait had time left: 0 for none, 1 for some and 2 for all of it.
// Nothing is left to release the object through, so a wait that nothing ends times out after 1
// second. The cases, for each kind:
// - the wait through the opened UID
// - the wait through the creating UID
// The waits are a semaphore wait, an event flag wait, a mutex lock while the main thread holds the
// mutex, a read lock while it holds the write lock, a message pipe receive, a simple event wait and
// a wait for a timer's event. This is a test of its own, since an implementation that frees the
// object under its waiter may crash.
//
// On a PS Vita TV on firmware 3.65 for every kind but the timer the delete of the creating UID
// works and leaves the waiter waiting, counted through the opened UID. The close of the opened UID
// then works and ends the wait with 0x80028006 (WAIT_DELETE), with time left, whichever UID it
// waits through. For a timer the delete works but makes the opened UID fail too: the waiter count
// and the close through it give 0x80028241 (UNKNOWN_TIMER_ID), and the wait isn't ended and times
// out with 0x80028005 (WAIT_TIMEOUT), whichever UID it waits through.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// Each case's own name, since a timer whose creating UID is deleted keeps its name taken
static char object_name[32];
#define NAME object_name
#define TIMEOUT 1000000

enum Kind { SEMA, EVENT_FLAG, MUTEX, RWLOCK, MSG_PIPE, SIMPLE_EVENT, TIMER, KIND_COUNT };
static const char *const kind_names[] = {
    "semaphore", "event flag", "mutex", "reader/writer lock", "message pipe", "simple event",
    "timer",
};

static volatile int kind;
static SceUID wait_uid;
static volatile int wait_result;
static volatile SceUInt left;

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TIMEOUT;
    char message[4];
    unsigned int bits;
    SceUInt32 pattern;
    SceUInt64 user_data;
    int result = -1;
    switch (kind) {
    case SEMA: result = sceKernelWaitSema(wait_uid, 1, &timeout); break;
    case EVENT_FLAG:
        result = sceKernelWaitEventFlag(wait_uid, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
        break;
    case MUTEX: result = sceKernelLockMutex(wait_uid, 1, &timeout); break;
    case RWLOCK: result = sceKernelLockReadRWLock(wait_uid, &timeout); break;
    case MSG_PIPE:
        result = sceKernelReceiveMsgPipe(wait_uid, message, sizeof(message), MSG_PIPE_MODE_FULL,
            NULL, &timeout);
        break;
    case SIMPLE_EVENT:
        result = sceKernelWaitEvent(wait_uid, EVENT_BIT_A, &pattern, &user_data, &timeout);
        break;
    case TIMER:
        result = sceKernelWaitEvent(wait_uid, EVENT_TIMER, &pattern, &user_data, &timeout);
        break;
    }
    left = timeout;
    wait_result = result;
    return 0;
}

// Creates the object, held by the main thread where it is a lock
static SceUID create(void) {
    const SceUInt32 attr = SCE_KERNEL_ATTR_OPENABLE;
    SceUID id = -1;
    switch (kind) {
    case SEMA: return sceKernelCreateSema(NAME, attr, 0, 16, NULL);
    case EVENT_FLAG: return sceKernelCreateEventFlag(NAME, attr, 0, NULL);
    case MUTEX: return sceKernelCreateMutex(NAME, attr, 1, NULL);
    case RWLOCK:
        id = sceKernelCreateRWLock(NAME, attr, NULL);
        sceKernelLockWriteRWLock(id, NULL);
        return id;
    case MSG_PIPE: return sceKernelCreateMsgPipe(NAME, MSG_PIPE_TYPE_USER, attr, 4096, NULL);
    case SIMPLE_EVENT: return sceKernelCreateSimpleEvent(NAME, attr, 0, NULL);
    case TIMER: return sceKernelCreateTimer(NAME, attr, NULL);
    }
    return id;
}

static SceUID open_object(void) {
    switch (kind) {
    case SEMA: return sceKernelOpenSema(NAME);
    case EVENT_FLAG: return sceKernelOpenEventFlag(NAME);
    case MUTEX: return sceKernelOpenMutex(NAME);
    case RWLOCK: return sceKernelOpenRWLock(NAME);
    case MSG_PIPE: return sceKernelOpenMsgPipe(NAME);
    case SIMPLE_EVENT: return sceKernelOpenSimpleEvent(NAME);
    case TIMER: return sceKernelOpenTimer(NAME);
    }
    return -1;
}

static int close_object(SceUID id) {
    switch (kind) {
    case SEMA: return sceKernelCloseSema(id);
    case EVENT_FLAG: return sceKernelCloseEventFlag(id);
    case MUTEX: return sceKernelCloseMutex(id);
    case RWLOCK: return sceKernelCloseRWLock(id);
    case MSG_PIPE: return sceKernelCloseMsgPipe(id);
    case SIMPLE_EVENT: return sceKernelCloseSimpleEvent(id);
    case TIMER: return sceKernelCloseTimer(id);
    }
    return -1;
}

static int delete_object(SceUID id) {
    switch (kind) {
    case SEMA: return sceKernelDeleteSema(id);
    case EVENT_FLAG: return sceKernelDeleteEventFlag(id);
    case MUTEX: return sceKernelDeleteMutex(id);
    case RWLOCK: return sceKernelDeleteRWLock(id);
    case MSG_PIPE: return sceKernelDeleteMsgPipe(id);
    case SIMPLE_EVENT: return sceKernelDeleteSimpleEvent(id);
    case TIMER: return sceKernelDeleteTimer(id);
    }
    return -1;
}

// The waiters the info call through id counts, or its error
static int waiters(SceUID id) {
    SceKernelSemaInfo sema = { .size = sizeof(sema) };
    SceKernelEventFlagInfo flag = { .size = sizeof(flag) };
    MutexInfo mutex = { .size = sizeof(mutex) };
    SceKernelRWLockInfo rwlock = { .size = sizeof(rwlock) };
    SceKernelMppInfo pipe = { .size = sizeof(pipe) };
    EventInfo event = { .size = sizeof(event) };
    TimerInfo timer = { .size = sizeof(timer) };
    int result = -1;
    switch (kind) {
    case SEMA: return (result = sceKernelGetSemaInfo(id, &sema)) < 0 ? result : sema.numWaitThreads;
    case EVENT_FLAG:
        return (result = sceKernelGetEventFlagInfo(id, &flag)) < 0 ? result : flag.numWaitThreads;
    case MUTEX:
        result = sceKernelGetMutexInfo(id, (SceKernelMutexInfo *)&mutex);
        return result < 0 ? result : (int)mutex.numWaitThreads;
    case RWLOCK:
        result = sceKernelGetRWLockInfo(id, &rwlock);
        return result < 0 ? result : (int)(rwlock.numReadWaitThreads + rwlock.numWriteWaitThreads);
    case MSG_PIPE:
        result = sceKernelGetMsgPipeInfo(id, &pipe);
        return result < 0 ? result : pipe.numReceiveWaitThreads;
    case SIMPLE_EVENT:
        result = sceKernelGetEventInfo(id, &event);
        return result < 0 ? result : (int)event.numWaitThreads;
    case TIMER:
        result = sceKernelGetTimerInfo(id, &timer);
        return result < 0 ? result : (int)timer.numWaitThreads;
    }
    return result;
}

static void run(int through_opened) {
    const char *name = kind_names[kind];
    const char *what =
        through_opened ? "waiting through the opened UID" : "waiting through the creating UID";
    sceClibSnprintf(object_name, sizeof(object_name), "Last %d %d", kind, through_opened);
    const SceUID id = create();
    const SceUID opened = open_object();
    PRINTF(id < 0 ? id : opened < 0 ? opened : 0, "%s, %s: create and open the object", name, what);
    wait_uid = through_opened ? opened : id;
    wait_result = 0xDEAD;
    left = 0xDEAD;

    const SceUID thread = test_start_thread(waiter);
    test_wait_until_waiting(thread);
    const int deleted = delete_object(id);
    // Read right after, so a wake the delete didn't do can't be missed
    const int counted = waiters(opened);
    const int closed = close_object(opened);
    test_join_thread(thread);

    PRINTF(deleted, "%s, %s: delete the creating UID", name, what);
    PRINTF(counted, "%s, %s: waiters through the opened UID after it", name, what);
    PRINTF(closed, "%s, %s: close the opened UID, the last", name, what);
    PRINTF(wait_result, "%s, %s: the wait", name, what);
    PRINTF(left == 0 ? 0 : left >= TIMEOUT ? 2 : 1, "%s, %s: the time left", name, what);
}

int main(void) {
    for (kind = 0; kind < KIND_COUNT; kind++) {
        run(1);
        run(0);
    }
    test_exit();
    return 0;
}
