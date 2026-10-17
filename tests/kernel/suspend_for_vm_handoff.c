// Whether a waiter that sceKernelSuspendThreadForVM holds gets the object at the wake, or only once
// it is resumed. thread_status_matrix shows a semaphore signal takes the count for such a waiter.
// W waits on core 0 with the VM attribute, and the main thread, on core 1, suspends it, wakes it,
// reads what the object holds and tries to take it itself, then resumes W. Kinds:
// - a mutex the main thread unlocks
// - a lightweight mutex the main thread unlocks
// - an event flag set, for a waiter with SCE_EVENT_WAITCLEAR
// - a write lock waiter, when the main thread unlocks its read lock
// - a read lock waiter, when the main thread unlocks its write lock
// - a message pipe receive of 4 bytes, and a send of 4
// - a condition signal, with the mutex free
// - a lightweight condition signal, with the mutex free
// - an auto reset simple event set
// - a thread signal, then a second one
// Each case prints the suspend's result, the wake's, W's status and wait type once it no longer
// waits, runs or is READY, or 200 ms after the wake, what the object holds and the main thread's
// take, then W's result after the resume, and the result of the release W makes after it.
//
// On a PS Vita TV on firmware 3.65 every wake hands the object to the waiter that the ForVM suspend
// holds, at the wake: W owns the mutex, the lightweight mutex and the write or read lock, the event
// flag's and the simple event's bits are taken, the pipe's bytes are received, and the condition
// and the lightweight condition give W their mutex. Right after, W reports SUSPENDED (0x100) with
// wait type 0, the main thread's own take fails, and once resumed W's wait returns 0. A second
// thread signal works too.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

typedef enum Kind {
    MUTEX,
    LW_MUTEX,
    FLAG,
    WRITER,
    READER,
    PIPE,
    COND,
    LW_COND,
    EVENT,
    SIGNAL
} Kind;

static Kind kind;
static SceUID object, mutex;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int result, release;

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    SceUInt32 bits;
    char buf[4];
    switch (kind) {
    case MUTEX:
        result = sceKernelLockMutex(object, 1, &timeout);
        release = sceKernelUnlockMutex(object, 1);
        break;
    case LW_MUTEX:
        result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        release = sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case FLAG:
        result = sceKernelWaitEventFlag(
            object, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &bits, &timeout);
        break;
    case WRITER:
        result = sceKernelLockWriteRWLock(object, &timeout);
        release = sceKernelUnlockWriteRWLock(object);
        break;
    case READER:
        result = sceKernelLockReadRWLock(object, &timeout);
        release = sceKernelUnlockReadRWLock(object);
        break;
    case PIPE:
        result =
            sceKernelReceiveMsgPipe(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout);
        break;
    case COND:
        sceKernelLockMutex(mutex, 1, NULL);
        result = sceKernelWaitCond(object, &timeout);
        release = sceKernelUnlockMutex(mutex, 1);
        break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        result = sceKernelWaitLwCond(&lw_cond, &timeout);
        release = sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case EVENT: result = sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout); break;
    case SIGNAL:
        result = sceKernelWaitSignal(0, 0, &timeout);
        // Takes a second signal if one is there
        timeout = 0;
        release = sceKernelWaitSignal(0, 0, &timeout);
        break;
    }
    return 0;
}

static SceUInt32 wait_type_of(Kind k) {
    switch (k) {
    case MUTEX: return WAIT_TYPE_MUTEX;
    case LW_MUTEX: return WAIT_TYPE_LW_MUTEX;
    case FLAG: return WAIT_TYPE_EVENT_FLAG;
    case WRITER:
    case READER: return WAIT_TYPE_RW_LOCK;
    case PIPE: return WAIT_TYPE_MSG_PIPE;
    case COND: return WAIT_TYPE_COND;
    case LW_COND: return WAIT_TYPE_LW_COND;
    case EVENT: return WAIT_TYPE_EVENT;
    case SIGNAL: return WAIT_TYPE_SIGNAL;
    }
    return 0;
}

// Whether thread is in a wait of the given type
static int waits_in(SceUID thread, SceUInt32 type) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType == type;
}

static void create_object(Kind k) {
    switch (k) {
    case MUTEX:
        object = sceKernelCreateMutex("mutex", 0, 0, NULL);
        sceKernelLockMutex(object, 1, NULL);
        break;
    case LW_MUTEX:
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case WRITER:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockReadRWLock(object, NULL);
        break;
    case READER:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    case COND:
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
        object = sceKernelCreateCond("cond", 0, mutex, NULL);
        break;
    case LW_COND:
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lwcond", 0, &lw_mutex, NULL);
        break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL); break;
    case SIGNAL: break;
    }
}

static int wake(Kind k, SceUID w) {
    SceSize size;
    switch (k) {
    case MUTEX: return sceKernelUnlockMutex(object, 1);
    case LW_MUTEX: return sceKernelUnlockLwMutex(&lw_mutex, 1);
    case FLAG: return sceKernelSetEventFlag(object, 0x1);
    case WRITER: return sceKernelUnlockReadRWLock(object);
    case READER: return sceKernelUnlockWriteRWLock(object);
    case PIPE: return sceKernelTrySendMsgPipe(object, "abcd", 4, MSG_PIPE_MODE_FULL, &size);
    case COND: return sceKernelSignalCond(object);
    case LW_COND: return sceKernelSignalLwCond(&lw_cond);
    case EVENT: return sceKernelSetEvent(object, EVENT_BIT_A, 0);
    case SIGNAL: return sceKernelSendSignal(w);
    }
    return 0;
}

// Prints what the object holds while W is suspended, and what a take by the main thread gets.
// Whatever the main thread takes it gives back, so W can still get it after the resume.
static void print_object(const char *name, Kind k, SceUID w) {
    switch (k) {
    case MUTEX:
    case COND: {
        const SceUID m = k == MUTEX ? object : mutex;
        MutexInfo info = { .size = sizeof(info) };
        sceKernelGetMutexInfo(m, (SceKernelMutexInfo *)&info);
        PRINTF(info.currentOwnerId == w, "%s: W owns the mutex", name);
        PRINTF(info.currentCount, "%s: the mutex's count", name);
        PRINTF(info.numWaitThreads, "%s: the mutex's waiters", name);
        const int try_lock = sceKernelTryLockMutex(m, 1);
        PRINTF(try_lock, "%s: try lock the mutex", name);
        if (try_lock == 0)
            sceKernelUnlockMutex(m, 1);
        break;
    }
    case LW_MUTEX:
    case LW_COND: {
        LwMutexInfo info = { .size = sizeof(info) };
        sceKernelGetLwMutexInfo(&lw_mutex, &info);
        PRINTF(info.currentOwnerId == w, "%s: W owns the lightweight mutex", name);
        PRINTF(info.currentCount, "%s: its count", name);
        PRINTF(info.numWaitThreads, "%s: its waiters", name);
        const int try_lock = sceKernelTryLockLwMutex(&lw_mutex, 1);
        PRINTF(try_lock, "%s: try lock it", name);
        if (try_lock == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    }
    case FLAG: {
        SceKernelEventFlagInfo info = { .size = sizeof(info) };
        sceKernelGetEventFlagInfo(object, &info);
        PRINTF(info.currentPattern, "%s: the pattern", name);
        PRINTF(info.numWaitThreads, "%s: the waiters", name);
        break;
    }
    case WRITER:
    case READER: {
        SceKernelRWLockInfo info = { .size = sizeof(info) };
        sceKernelGetRWLockInfo(object, &info);
        PRINTF(info.lockCount, "%s: the lock count", name);
        PRINTF(info.writeOwnerId == w, "%s: W is the write owner", name);
        PRINTF(info.numReadWaitThreads + info.numWriteWaitThreads, "%s: the waiters", name);
        if (k == WRITER) {
            const int try_read = sceKernelTryLockReadRWLock(object);
            PRINTF(try_read, "%s: try read lock", name);
            if (try_read == 0)
                sceKernelUnlockReadRWLock(object);
        } else {
            const int try_write = sceKernelTryLockWriteRWLock(object);
            PRINTF(try_write, "%s: try write lock", name);
            if (try_write == 0)
                sceKernelUnlockWriteRWLock(object);
        }
        break;
    }
    case PIPE: {
        SceKernelMppInfo info = { .size = sizeof(info) };
        sceKernelGetMsgPipeInfo(object, &info);
        PRINTF(info.freeSize, "%s: the free size", name);
        PRINTF(info.numReceiveWaitThreads, "%s: the receivers", name);
        break;
    }
    case EVENT: {
        EventInfo info = { .size = sizeof(info) };
        sceKernelGetEventInfo(object, &info);
        PRINTF(info.eventPattern & EVENT_BIT_A, "%s: the bit after the set", name);
        PRINTF(info.numWaitThreads, "%s: the waiters", name);
        break;
    }
    case SIGNAL: PRINTF(sceKernelSendSignal(w), "%s: a second signal", name); break;
    }
}

static void delete_object(Kind k) {
    switch (k) {
    case MUTEX: sceKernelDeleteMutex(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case WRITER:
    case READER: sceKernelDeleteRWLock(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    case COND:
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(mutex);
        break;
    case LW_COND:
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_mutex);
        break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case SIGNAL: break;
    }
}

static void run_case(const char *name, Kind k) {
    kind = k;
    result = release = 0xDEAD;
    create_object(k);
    const SceUID w = sceKernelCreateThread(
        "w", waiter, 0xA0, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(w, 0, NULL);
    TEST_WAIT_FOR(waits_in(w, wait_type_of(k)));
    const int suspended = sceKernelSuspendThreadForVM(w);
    const int woken = wake(k, w);
    // On a Vita the wake changes W's info before it returns, but a receiver woken by a send can
    // show READY until its core takes it and finds it suspended. This waits that out, and gives an
    // emulator that changes the info later the time to.
    SceKernelThreadInfo info = { .size = sizeof(info) };
    for (int i = 0; i < 200; i++) {
        sceKernelGetThreadInfo(w, &info);
        if (info.waitType == 0 && !(info.status & (SCE_THREAD_RUNNING | SCE_THREAD_READY)))
            break;
        sceKernelDelayThread(1000);
    }
    PRINTF(suspended, "%s: suspend W", name);
    PRINTF(woken, "%s: the wake", name);
    PRINTF(info.status, "%s: W's status after it", name);
    PRINTF(info.waitType, "%s: W's wait type after it", name);
    print_object(name, k, w);
    PRINTF(sceKernelResumeThreadForVM(w), "%s: resume W", name);
    test_join_thread(w);
    PRINTF(result, "%s: W's result", name);
    if (release != (int)0xDEAD)
        PRINTF(release,
            k == SIGNAL ? "%s: W's wait for a second signal" : "%s: W's release after it", name);
    delete_object(k);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("mutex unlock", MUTEX);
    run_case("lightweight mutex unlock", LW_MUTEX);
    run_case("event flag set", FLAG);
    run_case("read unlock to a writer", WRITER);
    run_case("write unlock to a reader", READER);
    run_case("message pipe send", PIPE);
    run_case("condition signal", COND);
    run_case("lightweight condition signal", LW_COND);
    run_case("simple event set", EVENT);
    run_case("thread signal", SIGNAL);

    test_exit();
    return 0;
}
