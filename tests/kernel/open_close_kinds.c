// For each kind of sync object with an open call, an openable object opened twice by its name. The
// cases: whether each open works and gives a UID of its own, odd and not the creating UID, and the
// class of the opened UIDs. The info call through each UID, which UID it reports and whether it
// has the object's name. A change made through the first opened UID, seen through each UID, and
// one made through the creating UID, seen through the opened ones. Then closing the creating UID,
// deleting through the first opened UID, and closing the second one twice, with the info call and
// the class through it after, and a change through the first one seen through the creating UID.
// Last, closing the first opened UID and deleting the creating one, and the info call through it
// after. A condition variable can't be openable, so for it the test only closes and deletes the
// creating UID.
//
// The state a change leaves is the semaphore's count, the event flag's bits, the mutex's lock
// count, the reader/writer lock's read lock count, the bytes in the message pipe, the simple
// event's pattern, and the stopped timer's time in milliseconds, which each change sets. Each is
// read after the info call through the UID worked, by the info call or by the pattern and time
// calls. The UID an info call reports prints 0 for the UID it was called with, 1 for the creating
// UID, 2 for the other opened UID and 3 for anything else.
//
// On a PS Vita TV on firmware 3.65 every kind opens twice, each open giving an odd UID of its own,
// and the second opened UID has the creating UID's class. The info call through any of the three
// UIDs has the name and reports the UID it was called with, and a change through any UID shows
// through the others. Closing the creating UID and deleting through an opened UID fail with the
// kind's unknown ID error, such as 0x80028101 (UNKNOWN_SEMA_ID), and leave that UID working.
// Closing an opened UID works once, then a second close and the info call through it give that
// error and its class call 0x80024501 (INVALID_UID), while the other UIDs still work. For a simple
// event the info call through a gone UID gives 0x80028201 (UNKNOWN_EVENT_ID), and its pattern holds
// 0x104 besides the bits the changes set, 0x304 by the end. Closing a condition variable's creating
// UID works, and its info call and delete then give 0x800281A1 (UNKNOWN_COND_ID).
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NAME "Opened"

enum Kind { SEMA, EVENT_FLAG, MUTEX, RWLOCK, MSG_PIPE, SIMPLE_EVENT, TIMER, KIND_COUNT };
static const char *const kind_names[] = {
    "semaphore", "event flag", "mutex", "reader/writer lock", "message pipe", "simple event",
    "timer",
};

// What every info call fills, with the fields they share first
typedef union Info {
    struct {
        SceSize size;
        SceUID id;
        char name[32];
    } head;
    SceKernelSemaInfo sema;
    SceKernelEventFlagInfo flag;
    MutexInfo mutex;
    SceKernelRWLockInfo rwlock;
    SceKernelMppInfo pipe;
    EventInfo event;
    TimerInfo timer;
} Info;

// The creating UID and the two opened ones, in that order
static SceUID uids[3];
static const char *const uid_names[] = {
    "the creating UID", "the first opened UID", "the second opened UID",
};

static SceUID create(int kind) {
    const SceUInt32 attr = SCE_KERNEL_ATTR_OPENABLE;
    switch (kind) {
    case SEMA: return sceKernelCreateSema(NAME, attr, 0, 16, NULL);
    case EVENT_FLAG: return sceKernelCreateEventFlag(NAME, attr, 0, NULL);
    case MUTEX: return sceKernelCreateMutex(NAME, attr | SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    case RWLOCK: return sceKernelCreateRWLock(NAME, attr, NULL);
    case MSG_PIPE: return sceKernelCreateMsgPipe(NAME, MSG_PIPE_TYPE_USER, attr, 4096, NULL);
    case SIMPLE_EVENT: return sceKernelCreateSimpleEvent(NAME, attr, 0, NULL);
    case TIMER: return sceKernelCreateTimer(NAME, attr, NULL);
    }
    return -1;
}

static SceUID open_object(int kind) {
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

static int close_object(int kind, SceUID id) {
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

static int delete_object(int kind, SceUID id) {
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

// Makes change number step, from 1, to the object through id
static int change(int kind, SceUID id, int step) {
    static char message[4];
    SceKernelSysClock clock = step * 1000;
    SceSize size;
    switch (kind) {
    case SEMA: return sceKernelSignalSema(id, 1);
    case EVENT_FLAG: return sceKernelSetEventFlag(id, 1u << step);
    case MUTEX: return sceKernelTryLockMutex(id, 1);
    case RWLOCK: return sceKernelTryLockReadRWLock(id);
    case MSG_PIPE:
        return sceKernelTrySendMsgPipe(id, message, sizeof(message), MSG_PIPE_MODE_ASAP, &size);
    case SIMPLE_EVENT: return sceKernelSetEvent(id, EVENT_BIT_A << (step - 1), 0);
    case TIMER: return sceKernelSetTimerTime(id, &clock);
    }
    return -1;
}

static int get_info(int kind, SceUID id, Info *info) {
    sceClibMemset(info, 0, sizeof(*info));
    switch (kind) {
    case SEMA:
        info->sema.size = sizeof(info->sema);
        return sceKernelGetSemaInfo(id, &info->sema);
    case EVENT_FLAG:
        info->flag.size = sizeof(info->flag);
        return sceKernelGetEventFlagInfo(id, &info->flag);
    case MUTEX:
        info->mutex.size = sizeof(info->mutex);
        return sceKernelGetMutexInfo(id, (SceKernelMutexInfo *)&info->mutex);
    case RWLOCK:
        info->rwlock.size = sizeof(info->rwlock);
        return sceKernelGetRWLockInfo(id, &info->rwlock);
    case MSG_PIPE:
        info->pipe.size = sizeof(info->pipe);
        return sceKernelGetMsgPipeInfo(id, &info->pipe);
    case SIMPLE_EVENT:
        info->event.size = sizeof(info->event);
        return sceKernelGetEventInfo(id, &info->event);
    case TIMER:
        info->timer.size = sizeof(info->timer);
        return sceKernelGetTimerInfo(id, &info->timer);
    }
    return -1;
}

static int info_result(int kind, SceUID id) {
    Info info;
    return get_info(kind, id, &info);
}

// The state the changes so far left, read through id, or the error of the call that reads it
static int state(int kind, SceUID id) {
    Info info;
    const int result = get_info(kind, id, &info);
    if (result < 0)
        return result;
    SceUInt32 pattern = 0;
    SceKernelSysClock clock = 0;
    int read;
    switch (kind) {
    case SEMA: return info.sema.currentCount;
    case EVENT_FLAG: return info.flag.currentPattern;
    case MUTEX: return info.mutex.currentCount;
    case RWLOCK: return info.rwlock.lockCount;
    case MSG_PIPE: return info.pipe.bufSize - info.pipe.freeSize;
    case SIMPLE_EVENT:
        read = sceKernelGetEventPattern(id, &pattern);
        return read < 0 ? read : (int)pattern;
    case TIMER:
        read = sceKernelGetTimerTime(id, &clock);
        return read < 0 ? read : (int)(clock / 1000);
    }
    return -1;
}

// 0 for self, 1 for the creating UID, 2 for the other opened UID, 3 for anything else
static int which_uid(SceUID seen, int self) {
    if (seen == uids[self])
        return 0;
    if (seen == uids[0])
        return 1;
    if (seen == uids[self == 1 ? 2 : 1])
        return 2;
    return 3;
}

static void run(int kind) {
    const char *name = kind_names[kind];
    const SceUID id = uids[0] = create(kind);
    PRINTF(test_created(id), "%s: create an openable object", name);
    const SceUID a = uids[1] = open_object(kind);
    const SceUID b = uids[2] = open_object(kind);
    PRINTF(test_created(a), "%s: open it", name);
    PRINTF(test_created(b), "%s: open it again", name);
    PRINTF(a != id && b != id, "%s: neither opened UID is the creating UID", name);
    PRINTF(a != b, "%s: the opened UIDs differ", name);
    PRINTF((a & 1) && (b & 1), "%s: the opened UIDs are odd", name);
    PRINTF(get_uid_class(a), "%s: the class of the first opened UID", name);
    PRINTF(get_uid_class(b) == get_uid_class(id), "%s: the second has the creating UID's class",
        name);

    for (int i = 0; i < 3; i++) {
        const char *through = uid_names[i];
        Info info;
        PRINTF(get_info(kind, uids[i], &info), "%s: info through %s", name, through);
        PRINTF(which_uid(info.head.id, i), "%s: the UID the info through %s reports", name,
            through);
        PRINTF(sceClibStrcmp(info.head.name, NAME) == 0, "%s: the info through %s has the name",
            name, through);
    }

    PRINTF(change(kind, a, 1), "%s: a change through the first opened UID", name);
    for (int i = 0; i < 3; i++)
        PRINTF(state(kind, uids[i]), "%s: the state through %s after it", name, uid_names[i]);
    PRINTF(change(kind, id, 2), "%s: a change through the creating UID", name);
    for (int i = 1; i < 3; i++)
        PRINTF(state(kind, uids[i]), "%s: the state through %s after it", name, uid_names[i]);

    PRINTF(close_object(kind, id), "%s: close the creating UID", name);
    PRINTF(info_result(kind, id), "%s: info through the creating UID after", name);
    PRINTF(delete_object(kind, a), "%s: delete through the first opened UID", name);
    PRINTF(info_result(kind, a), "%s: info through the first opened UID after", name);
    PRINTF(close_object(kind, b), "%s: close the second opened UID", name);
    PRINTF(close_object(kind, b), "%s: close it again", name);
    PRINTF(info_result(kind, b), "%s: info through it after", name);
    PRINTF(get_uid_class(b), "%s: its class after", name);
    PRINTF(change(kind, a, 3), "%s: a change through the first opened UID after", name);
    PRINTF(state(kind, id), "%s: the state through the creating UID after it", name);

    PRINTF(close_object(kind, a), "%s: close the first opened UID", name);
    PRINTF(delete_object(kind, id), "%s: delete the creating UID", name);
    PRINTF(info_result(kind, id), "%s: info through it after", name);
}

int main(void) {
    for (int kind = 0; kind < KIND_COUNT; kind++)
        run(kind);

    const SceUID mutex = sceKernelCreateMutex("cond mutex", 0, 0, NULL);
    const SceUID cond = sceKernelCreateCond(NAME, 0, mutex, NULL);
    SceKernelCondInfo info = { .size = sizeof(info) };
    PRINT("condition variable: close the creating UID", sceKernelCloseCond(cond));
    PRINT("condition variable: info through it after", sceKernelGetCondInfo(cond, &info));
    PRINT("condition variable: delete it", sceKernelDeleteCond(cond));
    sceKernelDeleteMutex(mutex);

    test_exit();
    return 0;
}
