// Whether a call that wakes a higher priority waiter on the same core switches to it before the
// call returns, for the wakes release_preempt leaves out: the cancel and the delete of an event
// flag, a mutex, a rwlock and a message pipe, a read unlock to a waiting writer, a pulse and a
// cancel of a simple event, a set that notifies callbacks, a set of an event a thread waits for
// with sceKernelWaitMultipleEvents, signal all and signal to of a condition and a lightweight
// condition whose mutex is free, a lightweight mutex delete, a timer cancel, and a ForVM resume
// of a waiter a semaphore signal woke while it was suspended. Both threads are pinned to core 0
// and the waiter is above the main thread. The main thread records 0 before the call and 2 after
// it, the waiter records 1 when its wait returns.
//
// On a PS Vita TV on firmware 3.65 every one of these calls switches to the waiter before it
// returns, so the steps go 0 1 2 in every case. The cancels end their waits with 0x80028007
// (WAIT_CANCEL) and the deletes with 0x80028006 (WAIT_DELETE), and the wait on several events
// returns 1.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0xA0
#define WAITER_PRIORITY 0x80
#define MAX_STEPS 4

typedef enum Kind {
    FLAG_CANCEL,
    FLAG_DELETE,
    MUTEX_CANCEL,
    MUTEX_DELETE,
    RWLOCK_CANCEL,
    RWLOCK_DELETE,
    READ_UNLOCK,
    PIPE_CANCEL,
    PIPE_DELETE,
    EVENT_PULSE,
    EVENT_CANCEL,
    EVENT_NOTIFY,
    MULTIPLE,
    COND_ALL,
    COND_TO,
    LW_COND_ALL,
    LW_COND_TO,
    LW_MUTEX_DELETE,
    TIMER_CANCEL,
    RESUME,
} Kind;

static Kind kind;
static SceUID object, mutex;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int result;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    SceUInt32 bits;
    char buf[4];
    switch (kind) {
    case FLAG_CANCEL:
    case FLAG_DELETE:
        result = sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
        break;
    case MUTEX_CANCEL:
    case MUTEX_DELETE: result = sceKernelLockMutex(object, 1, &timeout); break;
    case RWLOCK_CANCEL:
    case RWLOCK_DELETE:
    case READ_UNLOCK: result = sceKernelLockWriteRWLock(object, &timeout); break;
    case PIPE_CANCEL:
    case PIPE_DELETE:
        result =
            sceKernelReceiveMsgPipe(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout);
        break;
    case EVENT_PULSE:
    case EVENT_CANCEL:
    case EVENT_NOTIFY:
        result = sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout);
        break;
    case MULTIPLE: {
        WaitEvent events[1] = { { object, EVENT_BIT_A } };
        ResultEvent results[1];
        result = sceKernelWaitMultipleEvents(events, 1, EVENT_WAIT_MODE_OR, results, &timeout);
        break;
    }
    case COND_ALL:
    case COND_TO:
        sceKernelLockMutex(mutex, 1, NULL);
        result = sceKernelWaitCond(object, &timeout);
        record(1);
        sceKernelUnlockMutex(mutex, 1);
        return 0;
    case LW_COND_ALL:
    case LW_COND_TO:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        result = sceKernelWaitLwCond(&lw_cond, &timeout);
        record(1);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        return 0;
    case LW_MUTEX_DELETE: result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout); break;
    case TIMER_CANCEL:
        result = sceKernelWaitEvent(object, EVENT_TIMER, &bits, NULL, &timeout);
        break;
    case RESUME: result = sceKernelWaitSema(object, 1, &timeout); break;
    }
    record(1);
    return 0;
}

static void create_object(Kind k) {
    switch (k) {
    case FLAG_CANCEL:
    case FLAG_DELETE: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case MUTEX_CANCEL:
    case MUTEX_DELETE: object = sceKernelCreateMutex("mutex", 0, 1, NULL); break;
    case RWLOCK_CANCEL:
    case RWLOCK_DELETE:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case READ_UNLOCK:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockReadRWLock(object, NULL);
        break;
    case PIPE_CANCEL:
    case PIPE_DELETE:
        object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL);
        break;
    case EVENT_PULSE:
    case EVENT_CANCEL:
    case EVENT_NOTIFY:
    case MULTIPLE:
        object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL);
        break;
    case COND_ALL:
    case COND_TO:
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
        object = sceKernelCreateCond("cond", 0, mutex, NULL);
        break;
    case LW_COND_ALL:
    case LW_COND_TO:
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lwcond", 0, &lw_mutex, NULL);
        break;
    case LW_MUTEX_DELETE: sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 1, NULL); break;
    case TIMER_CANCEL: {
        object = sceKernelCreateTimer("timer", 0, NULL);
        // An event far past the end of the test, so only the cancel ends the wait
        SceUInt64 interval = 60000000;
        sceKernelSetTimerEvent(object, TIMER_TYPE_SET_EVENT, &interval, 0);
        sceKernelStartTimer(object);
        break;
    }
    case RESUME: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    }
}

static void delete_object(Kind k) {
    switch (k) {
    case FLAG_CANCEL: sceKernelDeleteEventFlag(object); break;
    case MUTEX_CANCEL: sceKernelDeleteMutex(object); break;
    case RWLOCK_CANCEL:
    case READ_UNLOCK: sceKernelDeleteRWLock(object); break;
    case PIPE_CANCEL: sceKernelDeleteMsgPipe(object); break;
    case EVENT_PULSE:
    case EVENT_CANCEL:
    case EVENT_NOTIFY:
    case MULTIPLE: sceKernelDeleteSimpleEvent(object); break;
    case COND_ALL:
    case COND_TO:
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(mutex);
        break;
    case LW_COND_ALL:
    case LW_COND_TO:
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_mutex);
        break;
    case TIMER_CANCEL: sceKernelDeleteTimer(object); break;
    case RESUME: sceKernelDeleteSema(object); break;
    default: break;
    }
}

static int wake(Kind k, SceUID thread) {
    SceUInt32 waiting;
    int senders, receivers, threads;
    switch (k) {
    case FLAG_CANCEL: return sceKernelCancelEventFlag(object, 0, &waiting);
    case FLAG_DELETE: return sceKernelDeleteEventFlag(object);
    case MUTEX_CANCEL: return sceKernelCancelMutex(object, 0, &threads);
    case MUTEX_DELETE: return sceKernelDeleteMutex(object);
    case RWLOCK_CANCEL: return sceKernelCancelRWLock(object, &waiting, &waiting, 0);
    case RWLOCK_DELETE: return sceKernelDeleteRWLock(object);
    case READ_UNLOCK: return sceKernelUnlockReadRWLock(object);
    case PIPE_CANCEL: return sceKernelCancelMsgPipe(object, &senders, &receivers);
    case PIPE_DELETE: return sceKernelDeleteMsgPipe(object);
    case EVENT_PULSE: return sceKernelPulseEvent(object, EVENT_BIT_A, 0);
    case EVENT_CANCEL: return sceKernelCancelEvent(object, &waiting);
    case EVENT_NOTIFY: return sceKernelSetEventWithNotifyCallback(object, EVENT_BIT_A, 0, 0);
    case MULTIPLE: return sceKernelSetEvent(object, EVENT_BIT_A, 0);
    case COND_ALL: return sceKernelSignalCondAll(object);
    case COND_TO: return sceKernelSignalCondTo(object, thread);
    case LW_COND_ALL: return sceKernelSignalLwCondAll(&lw_cond);
    case LW_COND_TO: return sceKernelSignalLwCondTo(&lw_cond, thread);
    case LW_MUTEX_DELETE: return sceKernelDeleteLwMutex(&lw_mutex);
    case TIMER_CANCEL: return sceKernelCancelTimer(object, &waiting);
    case RESUME: return sceKernelResumeThreadForVM(thread);
    }
    return 0;
}

static void run_case(const char *name, Kind k) {
    kind = k;
    result = 0xDEAD;
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
    create_object(k);
    const SceUID thread = sceKernelCreateThread("waiter", waiter, WAITER_PRIORITY, 0x4000,
        k == RESUME ? THREAD_ATTR_VM : 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    // The waiter runs at once, since it is above the main thread, until it waits
    sceKernelStartThread(thread, 0, NULL);
    test_wait_until_waiting(thread);
    if (k == RESUME) {
        sceKernelSuspendThreadForVM(thread);
        sceKernelSignalSema(object, 1);
    }
    record(0);
    const int call = wake(k, thread);
    record(2);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    sceKernelDeleteThread(thread);
    PRINTF(call, "%s: the call", name);
    PRINTF(result, "%s: the waiter's result", name);
    const int count = step_count < MAX_STEPS ? step_count : MAX_STEPS;
    for (int i = 0; i < count; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
    delete_object(k);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("event flag cancel", FLAG_CANCEL);
    run_case("event flag delete", FLAG_DELETE);
    run_case("mutex cancel", MUTEX_CANCEL);
    run_case("mutex delete", MUTEX_DELETE);
    run_case("rwlock cancel", RWLOCK_CANCEL);
    run_case("rwlock delete", RWLOCK_DELETE);
    run_case("read unlock to a writer", READ_UNLOCK);
    run_case("message pipe cancel", PIPE_CANCEL);
    run_case("message pipe delete", PIPE_DELETE);
    run_case("simple event pulse", EVENT_PULSE);
    run_case("simple event cancel", EVENT_CANCEL);
    run_case("simple event set with notify callback", EVENT_NOTIFY);
    run_case("simple event set to a wait on several events", MULTIPLE);
    run_case("condition signal all", COND_ALL);
    run_case("condition signal to", COND_TO);
    run_case("lightweight condition signal all", LW_COND_ALL);
    run_case("lightweight condition signal to", LW_COND_TO);
    run_case("lightweight mutex delete", LW_MUTEX_DELETE);
    run_case("timer cancel", TIMER_CANCEL);
    run_case("ForVM resume of a woken waiter", RESUME);

    test_exit();
    return 0;
}
