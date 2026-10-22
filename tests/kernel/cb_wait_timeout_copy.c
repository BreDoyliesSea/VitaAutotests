// What the timeout variable of a wait with callbacks holds while the callbacks run, and after the
// call returns. A thread T creates a callback, puts 2000000 in a global timeout variable and waits
// with callbacks, passing that variable as the timeout. Once T waits, the main thread notifies the
// callback. The callback records the value of the timeout variable and sets a flag. The main thread
// waits for the flag and for T to be back in its wait, gives T what it waits for and joins it.
// After the join it reads the timeout variable again. Every case runs on fresh objects and a fresh
// T. The cases differ in the wait:
// - a semaphore at 0, sceKernelWaitSemaCB, and the main thread signals it
// - an event flag with no bits set, sceKernelWaitEventFlagCB, and the main thread sets the bit
// - a mutex that the main thread holds, sceKernelLockMutexCB, and the main thread unlocks it
// - a lightweight mutex that the main thread holds, sceKernelLockLwMutexCB, the same way
// - a rwlock that the main thread write locks, sceKernelLockWriteRWLockCB, the same way
// - a condition, which T waits on while it holds the condition's mutex, sceKernelWaitCondCB, and
//   the main thread signals the condition
// - a simple event with no bits set, sceKernelWaitEventCB, and the main thread sets the bit
// - an empty message pipe, which T receives 4 bytes from in the FULL mode with
//   sceKernelReceiveMsgPipeCB, and the main thread sends 4 bytes
// Every thread is pinned to core 0, and T runs at 0xB0, below the main thread at 0xA0, so T runs its
// callback once the main thread blocks. Each case prints whether T waits with callbacks when the
// main thread notifies, T's wait result, whether the timeout that the callback saw is the one
// passed and whether it is above 1 s, and whether the timeout after the call is below the one
// passed and above 1 s.
//
// On a PS Vita TV on firmware 3.65 every wait returns 0, and the timeout after the call is below
// the one passed and above 1 s. The callback of the object waits sees the one passed, since those
// calls wait on a copy of the timeout and copy it back when they return. The message pipe call
// takes the caller's variable itself, and the notify writes the time left to it, so its callback
// sees a value below the one passed and above 1 s.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0xA0
#define T_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
#define TIMEOUT_US 2000000
#define ONE_SECOND_US 1000000

typedef enum Kind { SEMA, FLAG, MUTEX, LW_MUTEX, RWLOCK, COND, EVENT, PIPE, KIND_COUNT } Kind;

static const char *const kind_names[] = {
    "semaphore", "event flag", "mutex", "lightweight mutex", "rwlock write lock", "condition", "simple event", "message pipe receive",
};

static Kind kind;
// The object T waits on, and the condition's mutex
static SceUID object, mutex, callback;
static SceKernelLwMutexWork lw_mutex;
static SceUInt timeout_var;
static volatile int callback_ready, callback_ran, wait_result;
static volatile SceUInt callback_saw;

// Records the timeout variable's value
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_saw = *(volatile SceUInt *)&timeout_var;
    callback_ran = 1;
    return 0;
}

// Creates its callback, then waits with callbacks for the wait of the case
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 bits, pattern;
    SceUInt64 user_data;
    char buf[4];
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    timeout_var = TIMEOUT_US;
    callback_ready = 1;
    switch (kind) {
    case SEMA: wait_result = sceKernelWaitSemaCB(object, 1, &timeout_var); break;
    case FLAG: wait_result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout_var); break;
    case MUTEX:
        wait_result = sceKernelLockMutexCB(object, 1, &timeout_var);
        if (wait_result == 0)
            sceKernelUnlockMutex(object, 1);
        break;
    case LW_MUTEX:
        wait_result = sceKernelLockLwMutexCB(&lw_mutex, 1, &timeout_var);
        if (wait_result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case RWLOCK:
        wait_result = sceKernelLockWriteRWLockCB(object, &timeout_var);
        if (wait_result == 0)
            sceKernelUnlockWriteRWLock(object);
        break;
    case COND:
        sceKernelLockMutex(mutex, 1, NULL);
        wait_result = sceKernelWaitCondCB(object, &timeout_var);
        sceKernelUnlockMutex(mutex, 1);
        break;
    case EVENT: wait_result = sceKernelWaitEventCB(object, EVENT_BIT_A, &pattern, &user_data, &timeout_var); break;
    case PIPE: wait_result = sceKernelReceiveMsgPipeCB(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout_var); break;
    case KIND_COUNT: break;
    }
    sceKernelDeleteCallback(callback);
    return 0;
}

// Returns whether T is in the wait of the case, with callbacks. The lightweight mutex has no UID
// to compare.
static int t_waits(SceUID t) {
    switch (kind) {
    case SEMA: return test_in_wait(t, WAIT_TYPE_SEMA | WAIT_TYPE_CB, object);
    case FLAG: return test_in_wait(t, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, object);
    case MUTEX: return test_in_wait(t, WAIT_TYPE_MUTEX | WAIT_TYPE_CB, object);
    case LW_MUTEX: {
        SceKernelThreadInfo info = { .size = sizeof(info) };
        return sceKernelGetThreadInfo(t, &info) == 0 && info.waitType == (WAIT_TYPE_LW_MUTEX | WAIT_TYPE_CB);
    }
    case RWLOCK: return test_in_wait(t, WAIT_TYPE_RW_LOCK | WAIT_TYPE_CB, object);
    case COND: return test_in_wait(t, WAIT_TYPE_COND | WAIT_TYPE_CB, object);
    case EVENT: return test_in_wait(t, WAIT_TYPE_EVENT | WAIT_TYPE_CB, object);
    case PIPE: return test_in_wait(t, WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB, object);
    case KIND_COUNT: break;
    }
    return 0;
}

// Creates what the wait of the case needs, and takes what T waits for
static void setup(Kind k) {
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case MUTEX:
        object = sceKernelCreateMutex("mutex", 0, 0, NULL);
        sceKernelLockMutex(object, 1, NULL);
        break;
    case LW_MUTEX:
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        break;
    case RWLOCK:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    case COND:
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
        object = sceKernelCreateCond("cond", 0, mutex, NULL);
        break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL); break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    case KIND_COUNT: break;
    }
}

// Gives T what it waits for
static void release(Kind k) {
    SceSize size;
    switch (k) {
    case SEMA: sceKernelSignalSema(object, 1); break;
    case FLAG: sceKernelSetEventFlag(object, 0x1); break;
    case MUTEX: sceKernelUnlockMutex(object, 1); break;
    case LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case RWLOCK: sceKernelUnlockWriteRWLock(object); break;
    case COND: sceKernelSignalCond(object); break;
    case EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    case PIPE: sceKernelTrySendMsgPipe(object, "abcd", 4, MSG_PIPE_MODE_FULL, &size); break;
    case KIND_COUNT: break;
    }
}

// Deletes what setup created
static void cleanup(Kind k) {
    switch (k) {
    case SEMA: sceKernelDeleteSema(object); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case MUTEX: sceKernelDeleteMutex(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case RWLOCK: sceKernelDeleteRWLock(object); break;
    case COND:
        sceKernelDeleteCond(object);
        sceKernelDeleteMutex(mutex);
        break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    case KIND_COUNT: break;
    }
}

static void run_case(Kind k) {
    kind = k;
    wait_result = NOT_DONE;
    callback_ready = callback_ran = 0;
    callback_saw = NOT_DONE;
    timeout_var = NOT_DONE;
    setup(k);

    const SceUID t = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    // T starts when the main thread blocks here
    TEST_WAIT_FOR(callback_ready && t_waits(t));
    const int waits = t_waits(t);

    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(callback_ran);
    // T leaves its wait for the callback, so it has to be back before it is given what it waits for
    TEST_WAIT_FOR(t_waits(t));
    const SceUInt saw = callback_saw;

    release(k);
    test_join_thread(t);
    const SceUInt left = timeout_var;
    cleanup(k);

    const char *name = kind_names[k];
    PRINTF(waits, "%s: T waits with callbacks when the main thread notifies", name);
    PRINTF(wait_result, "%s: T's wait", name);
    PRINTF(saw == TIMEOUT_US, "%s: the timeout the callback saw is the one passed", name);
    PRINTF(saw > ONE_SECOND_US, "%s: the timeout the callback saw is above 1 s", name);
    PRINTF(left < TIMEOUT_US, "%s: the time left after the call is below the timeout passed", name);
    PRINTF(left > ONE_SECOND_US, "%s: the time left after the call is above 1 s", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    for (int k = 0; k < KIND_COUNT; k++)
        run_case((Kind)k);

    test_exit();
    return 0;
}
