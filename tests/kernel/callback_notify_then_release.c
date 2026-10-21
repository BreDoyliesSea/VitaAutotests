// Who gets the one unit of an object that a release hands out right after a callback notify. Thread
// T waits with callbacks at the head of the object's FIFO queue and thread W waits, without
// callbacks, behind it. The main thread notifies T's callback and then, without blocking in
// between, releases one unit of the object. Does W get the unit, since the notify took T out of the
// queue before the release, or does T? Does T's callback run, and what does T's wait return? Every
// case has a fresh object and fresh threads:
// - a semaphore with FIFO order, initial count 0 and max count 2. T calls sceKernelWaitSemaCB, W
//   sceKernelWaitSema, and the release is sceKernelSignalSema of 1
// - a mutex the main thread locked first. T calls sceKernelLockMutexCB, W sceKernelLockMutex, and
//   the release is sceKernelUnlockMutex
// - a lightweight mutex, the same with sceKernelLockLwMutexCB, sceKernelLockLwMutex and
//   sceKernelUnlockLwMutex
// - an event flag with SCE_EVENT_WAITMULTIPLE. T and W wait for bit 0x1 with SCE_EVENT_WAITOR and
//   SCE_EVENT_WAITCLEAR_PAT, T with sceKernelWaitEventFlagCB, and the release sets bit 0x1. Only
//   the first to take the bit gets it, since that wait clears it
// - a rwlock the main thread holds the write lock of. T calls sceKernelLockWriteRWLockCB, W
//   sceKernelLockWriteRWLock, and the release is sceKernelUnlockWriteRWLock
// - the semaphore case again, but T waits without callbacks, with sceKernelWaitSema. A notify can't
//   end a plain wait, so this case is the reference where T is expected to get the unit
//
// Every thread is pinned to core 0 and the main thread runs at 0x60, above T and W at 0xB0, so
// neither of them runs while the main thread doesn't block. T is started first and the main thread
// waits until it waits, then W is started the same way, which queues T before W. T creates its
// callback itself, which only counts its runs. The main thread notifies, releases, and reads the
// object's info and both thread statuses at once, before it blocks. Then it waits until T ran its
// callback and waits again, or is done, and for the semaphore and the event flag it releases a
// second unit so that whoever is left finishes. The lock cases need no second release, since a
// thread that got the lock unlocks it as soon as its wait returns. Every wait has a timeout of 6
// seconds, so a case can't hang. Nothing is printed until both threads ended.
//
// Each case prints these values:
// - whether T and W both wait, T first, before the notify
// - the result of the notify and of the release
// - the waiting threads the object's info counts right after the release
// - the semaphore's count, the lock's count or the flag's bits right after the release
// - for the locks, who owns the lock right after the release: 0 for nobody, 1 for W, 2 for T, 3 for
//   anyone else
// - the statuses of T and W right after the release (0x2 is ready, 0x8 is waiting)
// - who got the unit, from the state right after the release: 1 if W isn't waiting anymore, 2 if W
//   still waits and the unit is used up (the count is 0, the bits are cleared, or the lock has an
//   owner), 0 if W still waits and the unit is still there
// - for the semaphore and the event flag, whether T waits again after its callback ran, and the
//   result of the second release
// - how often T's callback ran when T's wait returned, the result of the sceKernelCheckCallback
//   that T calls after its wait, and how often the callback ran after that
// - the results of T's wait and of W's wait
//
// On a PS Vita TV on firmware 3.65 W gets the unit in every case with callbacks: the notify takes T
// out of the queue, so right after the release the object counts no waiting thread and both threads
// are ready. T's callback runs once the main thread blocks, then T waits again and gets the second
// release, or the lock once W gives it back. Without callbacks T keeps its place and gets the unit,
// W keeps waiting, and T's callback runs only at its sceKernelCheckCallback.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define THREAD_PRIORITY 0xB0
#define WAIT_TIMEOUT (3 * TEST_WAIT_US)
#define NOT_DONE 0xDEAD

typedef enum Kind { SEMA, MUTEX, LW_MUTEX, FLAG, RWLOCK, SEMA_PLAIN } Kind;

// What the main thread reads right after the release
typedef struct Snapshot {
    int waiters, level, owner, consumed, t_status, w_status;
} Snapshot;

static Kind kind;
static SceUID object, t_thread, w_thread;
static SceKernelLwMutexWork lw_mutex;
static volatile SceUID callback;
static volatile int callback_ready, callback_runs, t_done, w_done;
static volatile int t_result, w_result, runs_at_return, check_result, runs_after_check;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    return 0;
}

// Waits on the object of the case, with callbacks if asked to, and returns the wait's result
static int do_wait(int with_callbacks) {
    SceUInt timeout = WAIT_TIMEOUT;
    SceUInt32 bits = 0;
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: return with_callbacks ? sceKernelWaitSemaCB(object, 1, &timeout) : sceKernelWaitSema(object, 1, &timeout);
    case MUTEX: return with_callbacks ? sceKernelLockMutexCB(object, 1, &timeout) : sceKernelLockMutex(object, 1, &timeout);
    case LW_MUTEX: return with_callbacks ? sceKernelLockLwMutexCB(&lw_mutex, 1, &timeout) : sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
    case FLAG:
        return with_callbacks ? sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout)
                              : sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
    case RWLOCK: return with_callbacks ? sceKernelLockWriteRWLockCB(object, &timeout) : sceKernelLockWriteRWLock(object, &timeout);
    }
    return NOT_DONE;
}

// Unlocks what a wait that returned result took, if it took a lock
static void give_back(int result) {
    if (result != 0)
        return;
    switch (kind) {
    case MUTEX: sceKernelUnlockMutex(object, 1); break;
    case LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case RWLOCK: sceKernelUnlockWriteRWLock(object); break;
    default: break;
    }
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    callback_ready = 1;
    t_result = do_wait(kind != SEMA_PLAIN);
    runs_at_return = callback_runs;
    give_back(t_result);
    check_result = sceKernelCheckCallback();
    runs_after_check = callback_runs;
    sceKernelDeleteCallback(callback);
    t_done = 1;
    return 0;
}

static int w_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    w_result = do_wait(0);
    give_back(w_result);
    w_done = 1;
    return 0;
}

// Returns the wait type of the object of the case, without the callback bit
static SceUInt32 wait_type(void) {
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: return WAIT_TYPE_SEMA;
    case MUTEX: return WAIT_TYPE_MUTEX;
    case LW_MUTEX: return WAIT_TYPE_LW_MUTEX;
    case FLAG: return WAIT_TYPE_EVENT_FLAG;
    case RWLOCK: return WAIT_TYPE_RW_LOCK;
    }
    return 0;
}

// Returns whether thread waits on the object of the case with the given wait type
static int waits(SceUID thread, SceUInt32 type) {
    if (kind == LW_MUTEX) {
        SceKernelThreadInfo info = { .size = sizeof(info) };
        return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType == type;
    }
    return test_in_wait(thread, type, object);
}

static int thread_status(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 ? (int)info.status : NOT_DONE;
}

static int is_lock(void) {
    return kind == MUTEX || kind == LW_MUTEX || kind == RWLOCK;
}

// Returns whether the release leaves a unit that a thread still needs to take
static int needs_second_release(void) {
    return !is_lock();
}

static void create_object(void) {
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: object = sceKernelCreateSema("sema", 0, 0, 2, NULL); break;
    case MUTEX:
        object = sceKernelCreateMutex("mutex", 0, 0, NULL);
        sceKernelLockMutex(object, 1, NULL);
        break;
    case LW_MUTEX:
        sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        break;
    case FLAG: object = sceKernelCreateEventFlag("flag", SCE_EVENT_WAITMULTIPLE, 0, NULL); break;
    case RWLOCK:
        object = sceKernelCreateRWLock("rw", 0, NULL);
        sceKernelLockWriteRWLock(object, NULL);
        break;
    }
}

static void delete_object(void) {
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: sceKernelDeleteSema(object); break;
    case MUTEX: sceKernelDeleteMutex(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case RWLOCK: sceKernelDeleteRWLock(object); break;
    }
}

// Releases one unit of the object
static int release(void) {
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: return sceKernelSignalSema(object, 1);
    case MUTEX: return sceKernelUnlockMutex(object, 1);
    case LW_MUTEX: return sceKernelUnlockLwMutex(&lw_mutex, 1);
    case FLAG: return sceKernelSetEventFlag(object, 0x1);
    case RWLOCK: return sceKernelUnlockWriteRWLock(object);
    }
    return NOT_DONE;
}

// Returns 0 for no owner, 1 for W, 2 for T and 3 for anyone else
static int owner_code(SceUID owner) {
    if (owner == 0)
        return 0;
    return owner == w_thread ? 1 : owner == t_thread ? 2 : 3;
}

// Reads the object's info and the statuses of T and W
static Snapshot take_snapshot(void) {
    Snapshot s = { .waiters = NOT_DONE, .level = NOT_DONE, .owner = NOT_DONE, .consumed = 0 };
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: {
        SceKernelSemaInfo info = { .size = sizeof(info) };
        sceKernelGetSemaInfo(object, &info);
        s.waiters = info.numWaitThreads;
        s.level = info.currentCount;
        s.consumed = info.currentCount == 0;
        break;
    }
    case MUTEX: {
        MutexInfo info = { .size = sizeof(info) };
        sceKernelGetMutexInfo(object, (SceKernelMutexInfo *)&info);
        s.waiters = info.numWaitThreads;
        s.level = info.currentCount;
        s.owner = owner_code(info.currentOwnerId);
        s.consumed = info.currentOwnerId != 0;
        break;
    }
    case LW_MUTEX: {
        LwMutexInfo info = { .size = sizeof(info) };
        sceKernelGetLwMutexInfo(&lw_mutex, &info);
        s.waiters = info.numWaitThreads;
        s.level = info.currentCount;
        s.owner = owner_code(info.currentOwnerId);
        s.consumed = info.currentOwnerId != 0;
        break;
    }
    case FLAG: {
        SceKernelEventFlagInfo info = { .size = sizeof(info) };
        sceKernelGetEventFlagInfo(object, &info);
        s.waiters = info.numWaitThreads;
        s.level = info.currentPattern;
        s.consumed = info.currentPattern == 0;
        break;
    }
    case RWLOCK: {
        SceKernelRWLockInfo info = { .size = sizeof(info) };
        sceKernelGetRWLockInfo(object, &info);
        s.waiters = info.numWriteWaitThreads;
        s.level = info.lockCount;
        s.owner = owner_code(info.writeOwnerId);
        s.consumed = info.writeOwnerId != 0;
        break;
    }
    }
    s.t_status = thread_status(t_thread);
    s.w_status = thread_status(w_thread);
    return s;
}

static const char *level_name(void) {
    switch (kind) {
    case SEMA:
    case SEMA_PLAIN: return "the count";
    case FLAG: return "the bits";
    default: return "the lock count";
    }
}

// Starts a thread on core 0 below the main thread
static SceUID start(const char *name, SceKernelThreadEntry entry) {
    const SceUID thread = sceKernelCreateThread(name, entry, THREAD_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    return thread;
}

static void run_case(const char *name, Kind k) {
    kind = k;
    callback = 0;
    callback_ready = callback_runs = t_done = w_done = 0;
    t_result = w_result = runs_at_return = check_result = runs_after_check = NOT_DONE;
    create_object();

    const SceUInt32 t_type = k == SEMA_PLAIN ? wait_type() : wait_type() | WAIT_TYPE_CB;
    t_thread = start("t", t_entry);
    TEST_WAIT_FOR(callback_ready && waits(t_thread, t_type));
    const int t_waits = callback_ready && waits(t_thread, t_type);
    w_thread = start("w", w_entry);
    TEST_WAIT_FOR(waits(w_thread, wait_type()));
    const int w_waits = waits(w_thread, wait_type());

    // The notify, the release and the reads below must not block in between
    const int notify = sceKernelNotifyCallback(callback, 1);
    const int release_result = release();
    const Snapshot s = take_snapshot();
    int got = 0;
    if (!(s.w_status & SCE_THREAD_WAITING))
        got = 1;
    else if (s.consumed)
        got = 2;

    int again = 0;
    int second = NOT_DONE;
    if (needs_second_release()) {
        // T ran its callback and waits again, or is done, so the second unit isn't taken too early
        TEST_WAIT_FOR(t_done || (callback_runs > 0 && waits(t_thread, t_type)));
        again = !t_done && callback_runs > 0 && waits(t_thread, t_type);
        second = release();
    }
    TEST_WAIT_FOR(t_done && w_done);
    test_join_thread(t_thread);
    test_join_thread(w_thread);
    delete_object();

    PRINTF(t_waits && w_waits, "%s: T then W wait before the notify", name);
    PRINTF(notify, "%s: the notify", name);
    PRINTF(release_result, "%s: the release", name);
    PRINTF(s.waiters, "%s: the waiting threads after the release", name);
    PRINTF(s.level, "%s: %s after the release", name, level_name());
    if (is_lock())
        PRINTF(s.owner, "%s: the owner after the release", name);
    PRINTF(s.t_status, "%s: T's status after the release", name);
    PRINTF(s.w_status, "%s: W's status after the release", name);
    PRINTF(got, "%s: who got the unit", name);
    if (needs_second_release()) {
        PRINTF(again, "%s: T waits again after its callback", name);
        PRINTF(second, "%s: the second release", name);
    }
    PRINTF(runs_at_return, "%s: runs of T's callback when its wait returned", name);
    PRINTF(check_result, "%s: the check of T's callbacks after its wait", name);
    PRINTF(runs_after_check, "%s: runs of T's callback after the check", name);
    PRINTF(t_result, "%s: T's wait", name);
    PRINTF(w_result, "%s: W's wait", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("semaphore", SEMA);
    run_case("mutex", MUTEX);
    run_case("lightweight mutex", LW_MUTEX);
    run_case("event flag", FLAG);
    run_case("rwlock", RWLOCK);
    run_case("semaphore, T waits without callbacks", SEMA_PLAIN);

    test_exit();
    return 0;
}
