// What each cancel call writes to its counts of waiting threads when it fails. Every count variable
// is set to 0xDEAD before the call, and each call is made with:
// - a UID that doesn't exist, 0x12345
// - the UID of an object of its class that was deleted
// - the UID of an object of another class, a semaphore, or a mutex for the semaphore cancel
// - the UID of an object of its class with no waiters, which shows the count of a call that works
//
// The calls are sceKernelCancelSema, sceKernelCancelMutex, sceKernelCancelRWLock with both counts
// and flag 0, sceKernelCancelEventFlag, sceKernelCancelEvent and
// sceKernelCancelEventWithSetPattern on a simple event, sceKernelCancelMsgPipe with both counts,
// and sceKernelCancelTimer. Each prints its result and its counts.
//
// On a PS Vita TV on firmware 3.65 a failed cancel returns the error of its class, like the
// other calls do with a bad UID, and the counts read 0 after it, not 0xDEAD. A cancel of an object
// with no waiters returns 0 and writes 0 to every count.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { SEMA, MUTEX, RWLOCK, FLAG, EVENT, EVENT_SET, PIPE, TIMER, KINDS };

// What the output lines of each call start with
static const char *const calls[KINDS] = {
    "semaphore", "mutex", "reader/writer lock", "event flag", "simple event", "simple event with a set pattern", "message pipe", "timer",
};

// The class each call takes, for the line about a deleted object
static const char *const objects[KINDS] = {
    "a deleted semaphore", "a deleted mutex", "a deleted reader/writer lock", "a deleted event flag",
    "a deleted simple event", "a deleted simple event", "a deleted message pipe", "a deleted timer",
};

static SceUID create(int kind) {
    switch (kind) {
    case SEMA: return sceKernelCreateSema("sema", 0, 0, 3, NULL);
    case MUTEX: return sceKernelCreateMutex("mutex", 0, 0, NULL);
    case RWLOCK: return sceKernelCreateRWLock("rwlock", 0, NULL);
    case FLAG: return sceKernelCreateEventFlag("flag", 0, 0, NULL);
    case EVENT:
    case EVENT_SET: return sceKernelCreateSimpleEvent("event", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    case PIPE: return sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL);
    default: return sceKernelCreateTimer("timer", 0, NULL);
    }
}

static void destroy(int kind, SceUID id) {
    switch (kind) {
    case SEMA: sceKernelDeleteSema(id); break;
    case MUTEX: sceKernelDeleteMutex(id); break;
    case RWLOCK: sceKernelDeleteRWLock(id); break;
    case FLAG: sceKernelDeleteEventFlag(id); break;
    case EVENT:
    case EVENT_SET: sceKernelDeleteSimpleEvent(id); break;
    case PIPE: sceKernelDeleteMsgPipe(id); break;
    default: sceKernelDeleteTimer(id); break;
    }
}

// Calls the cancel of kind on id with its counts at 0xDEAD, and prints its result and its counts
static void cancel(int kind, SceUID id, const char *what) {
    const char *call = calls[kind];
    switch (kind) {
    case SEMA: {
        int waiting = 0xDEAD;
        PRINTF(sceKernelCancelSema(id, 0, &waiting), "%s: %s: cancel", call, what);
        PRINTF(waiting, "%s: %s: the waiting count", call, what);
        break;
    }
    case MUTEX: {
        int waiting = 0xDEAD;
        PRINTF(sceKernelCancelMutex(id, 0, &waiting), "%s: %s: cancel", call, what);
        PRINTF(waiting, "%s: %s: the waiting count", call, what);
        break;
    }
    case RWLOCK: {
        SceUInt32 readers = 0xDEAD, writers = 0xDEAD;
        PRINTF(sceKernelCancelRWLock(id, &readers, &writers, 0), "%s: %s: cancel", call, what);
        PRINTF(readers, "%s: %s: the waiting readers", call, what);
        PRINTF(writers, "%s: %s: the waiting writers", call, what);
        break;
    }
    case FLAG: {
        SceUInt32 waiting = 0xDEAD;
        PRINTF(sceKernelCancelEventFlag(id, 0, &waiting), "%s: %s: cancel", call, what);
        PRINTF(waiting, "%s: %s: the waiting count", call, what);
        break;
    }
    case EVENT: {
        SceUInt32 waiting = 0xDEAD;
        PRINTF(sceKernelCancelEvent(id, &waiting), "%s: %s: cancel", call, what);
        PRINTF(waiting, "%s: %s: the waiting count", call, what);
        break;
    }
    case EVENT_SET: {
        SceUInt32 waiting = 0xDEAD;
        PRINTF(sceKernelCancelEventWithSetPattern(id, EVENT_BIT_A, 0x77, &waiting), "%s: %s: cancel", call, what);
        PRINTF(waiting, "%s: %s: the waiting count", call, what);
        break;
    }
    case PIPE: {
        int senders = 0xDEAD, receivers = 0xDEAD;
        PRINTF(sceKernelCancelMsgPipe(id, &senders, &receivers), "%s: %s: cancel", call, what);
        PRINTF(senders, "%s: %s: the waiting senders", call, what);
        PRINTF(receivers, "%s: %s: the waiting receivers", call, what);
        break;
    }
    default: {
        SceUInt32 waiting = 0xDEAD;
        PRINTF(sceKernelCancelTimer(id, &waiting), "%s: %s: cancel", call, what);
        PRINTF(waiting, "%s: %s: the waiting count", call, what);
        break;
    }
    }
}

int main(void) {
    for (int kind = 0; kind < KINDS; kind++) {
        // A deleted object, then the others, so the deleted UID isn't taken again
        const SceUID deleted = create(kind);
        destroy(kind, deleted);
        const SceUID valid = create(kind);
        const int other_kind = kind == SEMA ? MUTEX : SEMA;
        const SceUID other = create(other_kind);

        cancel(kind, 0x12345, "an unknown UID");
        cancel(kind, deleted, objects[kind]);
        cancel(kind, other, kind == SEMA ? "a mutex" : "a semaphore");
        cancel(kind, valid, "no waiters");

        destroy(kind, valid);
        destroy(other_kind, other);
    }

    test_exit();
    return 0;
}
