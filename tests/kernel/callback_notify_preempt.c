// Whether sceKernelNotifyCallback switches to a higher priority thread that waits with callbacks
// before it returns, so that the thread runs its callback and goes back to its wait, or leaves the
// callback for when the main thread blocks. A thread T creates a callback and waits with callbacks,
// the main thread notifies that callback. Each wait is run twice, with T above the main thread and
// with T below it, on fresh objects and threads. The waits:
// - a semaphore at 0
// - an event flag whose bit isn't set
// - a mutex, a lightweight mutex and a rwlock write lock that the main thread holds
// - a simple event
// - a message pipe receive on an empty pipe
// - a condition, which T waits on while it holds the condition's mutex
// - a lightweight condition, the same way
// - the end of a helper thread, which waits on a semaphore until the case ends
// - a thread signal
// - a delay
// Every thread is pinned to core 0, and T is at 0x80 or 0xB0 against the main thread's 0xA0.
//
// The main thread records step 0 right before the notify and step 2 right after it, T's callback
// records step 1. Steps 0 1 2 mean the callback ran inside the notify, steps 0 2 1 mean it ran
// after the notify, once the main thread blocked. Right after the notify, before anything blocks,
// the main thread reads T's status, its wait type and whether it is in its wait with callbacks,
// which a thread that ran its callback and went back to the wait is. Then the test waits for the
// callback to have run and for T to be in its wait again, ends the wait (a signal, a set, an
// unlock, a send, the end of the helper thread, or the delay's end) and joins T. Values are printed
// once the case is over, since printing lets T run.
//
// On a PS Vita TV on firmware 3.65 every kind behaves the same. With T above the main thread the
// steps are 0 1 2: the notify switches to T, which runs its callback and is back in its wait with
// callbacks when the notify returns. With T below it the steps are 0 2 1: right after the notify T
// is ready with wait type 0, and it runs its callback once the main thread blocks. A condition
// waiter goes back to its wait on the condition, not on the mutex.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0xA0
#define ABOVE_PRIORITY 0x80
#define BELOW_PRIORITY 0xB0
#define STEP_COUNT 3
#define NOT_DONE 0xDEAD

typedef enum Kind { SEMA, FLAG, MUTEX, LW_MUTEX, RWLOCK, EVENT, PIPE, COND, LW_COND, THREAD_END, SIGNAL, DELAY, KIND_COUNT } Kind;

static const char *const kind_names[] = {
    "semaphore", "event flag", "mutex", "lightweight mutex", "rwlock write lock", "simple event", "message pipe receive",
    "condition", "lightweight condition", "thread end", "thread signal", "delay",
};

static Kind kind;
// The object T waits on, or for a thread end wait the helper thread. mutex is the condition's.
static SceUID object, mutex, helper_sema, callback;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int callback_ready, callback_runs, wait_result;
static int step_count, steps[STEP_COUNT];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < STEP_COUNT)
        steps[i] = step;
}

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    record(1);
    return 0;
}

// Waits on the semaphore until the case ends
static int helper_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 5 * TEST_WAIT_US;
    sceKernelWaitSema(helper_sema, 1, &timeout);
    return 0;
}

// Creates its callback, then waits with callbacks for the wait of the case
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    SceUInt32 bits, pattern;
    SceUInt64 user_data;
    int status;
    char buf[4];
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    callback_ready = 1;
    switch (kind) {
    case SEMA: wait_result = sceKernelWaitSemaCB(object, 1, &timeout); break;
    case FLAG: wait_result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case MUTEX:
        wait_result = sceKernelLockMutexCB(object, 1, &timeout);
        if (wait_result == 0)
            sceKernelUnlockMutex(object, 1);
        break;
    case LW_MUTEX:
        wait_result = sceKernelLockLwMutexCB(&lw_mutex, 1, &timeout);
        if (wait_result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case RWLOCK:
        wait_result = sceKernelLockWriteRWLockCB(object, &timeout);
        if (wait_result == 0)
            sceKernelUnlockWriteRWLock(object);
        break;
    case EVENT: wait_result = sceKernelWaitEventCB(object, EVENT_BIT_A, &pattern, &user_data, &timeout); break;
    case PIPE: wait_result = sceKernelReceiveMsgPipeCB(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    case COND:
        sceKernelLockMutex(mutex, 1, NULL);
        wait_result = sceKernelWaitCondCB(object, &timeout);
        sceKernelUnlockMutex(mutex, 1);
        break;
    case LW_COND:
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        wait_result = sceKernelWaitLwCondCB(&lw_cond, &timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case THREAD_END: wait_result = sceKernelWaitThreadEndCB(object, &status, &timeout); break;
    case SIGNAL: wait_result = sceKernelWaitSignalCB(0, 0, &timeout); break;
    case DELAY: wait_result = sceKernelDelayThreadCB(TEST_WAIT_US); break;
    case KIND_COUNT: break;
    }
    sceKernelDeleteCallback(callback);
    return 0;
}

// Returns whether the thread info says that its thread is in the wait of the case, with callbacks.
// The lightweight objects, the thread signal and the delay have no UID to compare.
static int info_in_wait(const SceKernelThreadInfo *info) {
    switch (kind) {
    case SEMA: return info->waitType == (WAIT_TYPE_SEMA | WAIT_TYPE_CB) && info->waitId == object;
    case FLAG: return info->waitType == (WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB) && info->waitId == object;
    case MUTEX: return info->waitType == (WAIT_TYPE_MUTEX | WAIT_TYPE_CB) && info->waitId == object;
    case LW_MUTEX: return info->waitType == (WAIT_TYPE_LW_MUTEX | WAIT_TYPE_CB);
    case RWLOCK: return info->waitType == (WAIT_TYPE_RW_LOCK | WAIT_TYPE_CB) && info->waitId == object;
    case EVENT: return info->waitType == (WAIT_TYPE_EVENT | WAIT_TYPE_CB) && info->waitId == object;
    case PIPE: return info->waitType == (WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB) && info->waitId == object;
    case COND: return info->waitType == (WAIT_TYPE_COND | WAIT_TYPE_CB) && info->waitId == object;
    case LW_COND: return info->waitType == (WAIT_TYPE_LW_COND | WAIT_TYPE_CB);
    case THREAD_END: return info->waitType == (WAIT_TYPE_THREAD_END | WAIT_TYPE_CB) && info->waitId == object;
    case SIGNAL: return info->waitType == (WAIT_TYPE_SIGNAL | WAIT_TYPE_CB);
    case DELAY: return info->waitType == (WAIT_TYPE_DELAY | WAIT_TYPE_CB);
    case KIND_COUNT: break;
    }
    return 0;
}

static int t_waits(SceUID t) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(t, &info) == 0 && info_in_wait(&info);
}

// Creates what the wait of the case needs, and for the thread end wait starts the helper thread
static void setup(Kind k) {
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case MUTEX: object = sceKernelCreateMutex("mutex", 0, 1, NULL); break;
    case LW_MUTEX: sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 1, NULL); break;
    case RWLOCK:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL); break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    case COND:
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
        object = sceKernelCreateCond("cond", 0, mutex, NULL);
        break;
    case LW_COND:
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lwcond", 0, &lw_mutex, NULL);
        break;
    case THREAD_END:
        helper_sema = sceKernelCreateSema("helper", 0, 0, 1, NULL);
        object = sceKernelCreateThread("helper", helper_entry, ABOVE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(object, 0, NULL);
        TEST_WAIT_FOR(test_in_wait(object, WAIT_TYPE_SEMA, helper_sema));
        break;
    case SIGNAL:
    case DELAY:
    case KIND_COUNT: break;
    }
}

// Ends the wait of T, except a delay's, which ends by itself
static void end_wait(Kind k, SceUID t) {
    SceSize size;
    switch (k) {
    case SEMA: sceKernelSignalSema(object, 1); break;
    case FLAG: sceKernelSetEventFlag(object, 0x1); break;
    case MUTEX: sceKernelUnlockMutex(object, 1); break;
    case LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case RWLOCK: sceKernelUnlockWriteRWLock(object); break;
    case EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    case PIPE: sceKernelTrySendMsgPipe(object, "abcd", 4, MSG_PIPE_MODE_FULL, &size); break;
    case COND: sceKernelSignalCond(object); break;
    case LW_COND: sceKernelSignalLwCond(&lw_cond); break;
    case THREAD_END: sceKernelSignalSema(helper_sema, 1); break;
    case SIGNAL: sceKernelSendSignal(t); break;
    case DELAY:
    case KIND_COUNT: break;
    }
}

// Waits for a thread to end, for a few seconds at most, and deletes it
static void join(SceUID thread) {
    SceUInt timeout = 5 * TEST_WAIT_US;
    sceKernelWaitThreadEnd(thread, NULL, &timeout);
    sceKernelDeleteThread(thread);
}

// Deletes what setup created, and joins the helper thread
static void cleanup(Kind k) {
    switch (k) {
    case SEMA: sceKernelDeleteSema(object); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case MUTEX: sceKernelDeleteMutex(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case RWLOCK: sceKernelDeleteRWLock(object); break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    case COND:
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(mutex);
        break;
    case LW_COND:
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_mutex);
        break;
    case THREAD_END:
        join(object);
        sceKernelDeleteSema(helper_sema);
        break;
    case SIGNAL:
    case DELAY:
    case KIND_COUNT: break;
    }
}

static void run_case(Kind k, int above) {
    kind = k;
    wait_result = NOT_DONE;
    callback_ready = callback_runs = 0;
    step_count = 0;
    for (int i = 0; i < STEP_COUNT; i++)
        steps[i] = -1;
    setup(k);

    const SceUID t = sceKernelCreateThread("t", t_entry, above ? ABOVE_PRIORITY : BELOW_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    // A T below the main thread starts when the main thread blocks here
    TEST_WAIT_FOR(callback_ready && t_waits(t));
    const int waits_before = t_waits(t);

    record(0);
    const int notify = sceKernelNotifyCallback(callback, 1);
    record(2);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(t, &info);
    const int status = info.status;
    const int wait_type = info.waitType;
    const int waits_after_notify = info_in_wait(&info);

    TEST_WAIT_FOR(callback_runs != 0);
    TEST_WAIT_FOR(t_waits(t));
    const int waits_again = t_waits(t);
    end_wait(k, t);
    join(t);
    cleanup(k);

    char name[64];
    sceClibSnprintf(name, sizeof(name), "%s, T %s the main thread", kind_names[k], above ? "above" : "below");
    PRINTF(waits_before, "%s: T waits with callbacks before the notify", name);
    for (int i = 0; i < STEP_COUNT; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
    PRINTF(notify, "%s: the notify", name);
    PRINTF(status, "%s: T's status right after the notify", name);
    PRINTF(wait_type, "%s: T's wait type right after the notify", name);
    PRINTF(waits_after_notify, "%s: T waits with callbacks right after the notify", name);
    PRINTF(callback_runs, "%s: runs of the callback", name);
    PRINTF(waits_again, "%s: T waits with callbacks after the callback ran", name);
    PRINTF(wait_result, "%s: T's wait", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    for (int k = 0; k < KIND_COUNT; k++) {
        run_case((Kind)k, 1);
        run_case((Kind)k, 0);
    }

    test_exit();
    return 0;
}
