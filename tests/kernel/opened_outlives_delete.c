// For each kind of sync object with an open call, how an openable object lives on through the UIDs
// opened from it once its creating UID is deleted. The object is opened twice by its name, changed
// through the creating UID, and then that UID is deleted. The cases: the class of the deleted UID,
// the info call and a second delete through it, then through the first opened UID the info call,
// the state, which UID the info reports, a wait that ends at once and a change, with the state seen
// through the second opened UID. Then, while the opened UIDs keep the object, opening its name,
// creating an openable object of the kind with the name, and which object an open of the name gives
// then, creating an openable object of another kind with the name, and one of the kind that isn't
// openable. Then closing the first opened UID, the info call and the state through the second, and
// closing the second, the last UID of the object, with the info call through it after. Last,
// opening the name and creating an openable object with it once the object has no UID left.
//
// The state a change leaves is the semaphore's count, the event flag's bits, the mutex's lock
// count, the reader/writer lock's read lock count, the bytes in the message pipe, the simple
// event's pattern, and the stopped timer's time in milliseconds, which each change sets. Each is
// read after the info call through the UID worked, by the info call or by the pattern and time
// calls. The wait that ends at once takes one off the semaphore, waits for the event flag's bit
// and the simple event's bit from the first change without clearing them, locks the mutex and read
// locks the reader/writer lock once more, and receives the bytes of the first change from the
// message pipe. For the timer it sets the time to 0 and an event 1 ms ahead, starts the timer,
// waits for the event for up to 2 seconds and stops the timer. The UID an info call reports prints
// 0 for the UID it was called with, 1 for the deleted creating UID, 2 for the other opened UID and
// 3 for anything else.
//
// On a PS Vita TV on firmware 3.65 the deleted creating UID is gone, with 0x80024501 (INVALID_UID)
// from the class call and the kind's unknown ID error, such as 0x80028101 (UNKNOWN_SEMA_ID), from
// the info call and a second delete. For every kind but the timer the object lives on through the
// opened UIDs, the info call reporting the UID it was called with, the wait ending at once and a
// change showing through the other opened UID. While they keep it the name still opens, and an
// openable object of any kind can't take the name, with 0x80024E02 (DUPLICATE_NAME), but one that
// isn't openable can. After the last close the name no longer opens, with 0x8002450A
// (UID_CANNOT_FIND_BY_NAME), and a new openable object takes it. A timer's opened UIDs stop working
// with the delete, every call through them, the closes too, giving 0x80028241 (UNKNOWN_TIMER_ID),
// yet its name still opens after both closes and a new openable timer can't take it.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NAME "Kept"

enum Kind { SEMA, EVENT_FLAG, MUTEX, RWLOCK, MSG_PIPE, SIMPLE_EVENT, TIMER, KIND_COUNT };
static const char *const kind_names[] = {
    "semaphore", "event flag", "mutex", "reader/writer lock", "message pipe", "simple event",
    "timer",
};

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

static SceUID create(int kind, SceUInt32 attr) {
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

// A wait through id that the first change lets end at once, or soon for the timer
static int wait_once(int kind, SceUID id) {
    SceUInt timeout = TEST_WAIT_US;
    char message[4];
    unsigned int bits;
    SceUInt32 pattern;
    SceUInt64 user_data;
    SceUInt64 interval = 1000;
    SceKernelSysClock clock = 0;
    int result;
    switch (kind) {
    case SEMA: return sceKernelWaitSema(id, 1, &timeout);
    case EVENT_FLAG: return sceKernelWaitEventFlag(id, 1u << 1, SCE_EVENT_WAITOR, &bits, &timeout);
    case MUTEX: return sceKernelLockMutex(id, 1, &timeout);
    case RWLOCK: return sceKernelLockReadRWLock(id, &timeout);
    case MSG_PIPE:
        return sceKernelReceiveMsgPipe(id, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL,
            &timeout);
    case SIMPLE_EVENT: return sceKernelWaitEvent(id, EVENT_BIT_A, &pattern, &user_data, &timeout);
    case TIMER:
        // From a time of 0, like a new timer
        result = sceKernelSetTimerTime(id, &clock);
        if (result == 0)
            result = sceKernelSetTimerEvent(id, TIMER_TYPE_SET_EVENT, &interval, 0);
        if (result == 0)
            result = sceKernelStartTimer(id);
        if (result == 0)
            result = sceKernelWaitEvent(id, EVENT_TIMER, &pattern, &user_data, &timeout);
        sceKernelStopTimer(id);
        return result;
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

static void run(int kind) {
    const char *name = kind_names[kind];
    const SceUID id = create(kind, SCE_KERNEL_ATTR_OPENABLE);
    const SceUID a = open_object(kind);
    const SceUID b = open_object(kind);
    PRINTF(id < 0 ? id : a < 0 ? a : b < 0 ? b : 0,
        "%s: create an openable object and open it twice", name);
    PRINTF(change(kind, id, 1), "%s: a change through the creating UID", name);
    PRINTF(delete_object(kind, id), "%s: delete the creating UID", name);
    PRINTF(get_uid_class(id), "%s: the class of the deleted UID", name);
    PRINTF(info_result(kind, id), "%s: info through it", name);
    PRINTF(delete_object(kind, id), "%s: delete it again", name);

    Info info;
    PRINTF(get_info(kind, a, &info), "%s: info through the first opened UID", name);
    const SceUID seen = info.head.id;
    PRINTF(seen == a ? 0 : seen == id ? 1 : seen == b ? 2 : 3, "%s: the UID it reports", name);
    PRINTF(state(kind, a), "%s: the state through it", name);
    PRINTF(wait_once(kind, a), "%s: a wait through it that can end at once", name);
    PRINTF(change(kind, a, 2), "%s: a change through it", name);
    PRINTF(state(kind, b), "%s: the state through the second opened UID after", name);

    // The name, while the opened UIDs keep the object
    const SceUID again = open_object(kind);
    PRINTF(test_created(again), "%s: open the name", name);
    if (again >= 0)
        close_object(kind, again);
    const SceUID same = create(kind, SCE_KERNEL_ATTR_OPENABLE);
    PRINTF(test_created(same), "%s: create another openable object with the name", name);
    if (same >= 0) {
        const SceUID opened = open_object(kind);
        // The new object has had no changes, unlike the one the opened UIDs keep
        const int fresh = opened >= 0 && state(kind, opened) != state(kind, b);
        PRINTF(opened < 0 ? opened : fresh, "%s: an open of the name then gives the new object",
            name);
        if (opened >= 0)
            close_object(kind, opened);
        delete_object(kind, same);
    }
    const int other_kind = (kind + 1) % KIND_COUNT;
    const SceUID other = create(other_kind, SCE_KERNEL_ATTR_OPENABLE);
    PRINTF(test_created(other), "%s: create an openable %s with the name", name,
        kind_names[other_kind]);
    if (other >= 0)
        delete_object(other_kind, other);
    const SceUID plain = create(kind, 0);
    PRINTF(test_created(plain), "%s: create one that isn't openable with the name", name);
    if (plain >= 0)
        delete_object(kind, plain);

    PRINTF(close_object(kind, a), "%s: close the first opened UID", name);
    PRINTF(info_result(kind, b), "%s: info through the second opened UID", name);
    PRINTF(state(kind, b), "%s: the state through it", name);
    PRINTF(close_object(kind, b), "%s: close it, the object's last UID", name);
    PRINTF(info_result(kind, b), "%s: info through it after", name);

    // The name, once the object has no UID left
    const SceUID late = open_object(kind);
    PRINTF(test_created(late), "%s: open the name after the last close", name);
    if (late >= 0)
        close_object(kind, late);
    const SceUID last = create(kind, SCE_KERNEL_ATTR_OPENABLE);
    PRINTF(test_created(last), "%s: create an openable object with the name after it", name);
    if (last >= 0)
        delete_object(kind, last);
}

int main(void) {
    for (int kind = 0; kind < KIND_COUNT; kind++)
        run(kind);
    test_exit();
    return 0;
}
