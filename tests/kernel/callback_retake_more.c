// What a wait with callbacks does when a callback of the waiting thread takes the lock that the
// thread waits for. Thread T waits with callbacks, and its callback takes the lock and returns
// while it holds it. T's wait then either takes the lock as well or waits on it again. Two kinds of
// cases:
// - rwlock cases, with the callback notified while T waits. The main thread holds the write lock
//   and T waits with sceKernelLockWriteRWLockCB or sceKernelLockReadRWLockCB. The main thread
//   notifies T's callback, which blocks on a gate semaphore. The main thread unlocks the write
//   lock and signals the gate, and the callback takes the write lock or a read lock:
//   - writer whose callback takes the write lock
//   - writer whose callback takes a read lock
//   - reader whose callback takes the write lock
//   - reader whose callback takes a read lock
// - cases with the callback already pending when T starts its wait. T notifies its own callback,
//   then waits with callbacks on a lock that is free, and the callback takes the lock before T's
//   wait does:
//   - mutex: sceKernelLockMutexCB, and the callback locks the mutex
//   - rwlock: sceKernelLockWriteRWLockCB, and the callback takes the write lock
//
// Every thread is pinned to core 0. The main thread runs at 0x60 and T at 0xB0. T's wait has a
// timeout of 1 s, far longer than the callback takes, and every other wait 20 s, so a wrong result
// can't hang the test. Every case has fresh objects and a fresh T. Right after its wait, T reads
// the lock's info, then gives back every lock it holds. Each case prints the result of the
// callback's lock, T's wait result, the lock count after T's wait and whether T owns the write lock
// or the mutex after it. The notified cases also print whether T waits on the rwlock before the
// notify, on the gate after it, and on the rwlock again after its callback.
//
// On a PS Vita TV on firmware 3.65 a lock that T's own callback took blocks T's wait like anyone
// else's. A writer waits again after its callback whichever lock the callback took, and so does a
// reader whose callback took the write lock. Those waits time out (0x80028005) with T holding the
// callback's lock. A reader whose callback took a read lock gets its own at once, with a count of
// 2. With the callback pending at the start, T's mutex and write lock waits don't treat the lock
// the callback took as a lock of their own: they wait on it and time out too, instead of failing as
// a recursive lock.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define THREAD_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
// Longer than the 5 s the main thread waits for anything, so a waiter is still waiting then
#define LONG_TIMEOUT (4 * TEST_WAIT_US)
// T's wait, far longer than the callback takes
#define T_TIMEOUT 1000000

typedef enum Lock { MUTEX, READ_LOCK, WRITE_LOCK } Lock;

// What T waits for, what its callback takes, and whether the callback is pending before T waits
static Lock t_lock, callback_take;
static int pending;
static SceUID mutex, rwlock, gate;
static volatile SceUID callback;
static volatile int callback_lock, callback_done, t_wait, t_count, t_owns;
static int saw_wait, saw_gate;

// Blocks on the gate unless it was pending before T's wait, then takes the lock of the case and
// returns holding it
static int take_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = LONG_TIMEOUT;
    if (!pending)
        sceKernelWaitSema(gate, 1, &timeout);
    timeout = LONG_TIMEOUT;
    if (callback_take == MUTEX)
        callback_lock = sceKernelLockMutex(mutex, 1, &timeout);
    else if (callback_take == WRITE_LOCK)
        callback_lock = sceKernelLockWriteRWLock(rwlock, &timeout);
    else
        callback_lock = sceKernelLockReadRWLock(rwlock, &timeout);
    callback_done = 1;
    return 0;
}

// Creates its callback, notifies it first if the case says so, waits on the lock with callbacks,
// reads the lock's state and gives back what it holds
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const SceUID self = sceKernelGetThreadId();
    callback = sceKernelCreateCallback("take", 0, take_callback, NULL);
    if (pending)
        sceKernelNotifyCallback(callback, 1);
    SceUInt timeout = T_TIMEOUT;
    if (t_lock == MUTEX) {
        t_wait = sceKernelLockMutexCB(mutex, 1, &timeout);
        MutexInfo info = { .size = sizeof(info) };
        sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
        t_count = info.currentCount;
        t_owns = info.currentOwnerId == self;
        if (t_owns) {
            for (int i = 0; i < info.currentCount; i++)
                sceKernelUnlockMutex(mutex, 1);
        }
    } else {
        t_wait = t_lock == WRITE_LOCK ? sceKernelLockWriteRWLockCB(rwlock, &timeout) : sceKernelLockReadRWLockCB(rwlock, &timeout);
        SceKernelRWLockInfo info = { .size = sizeof(info) };
        sceKernelGetRWLockInfo(rwlock, &info);
        t_count = info.lockCount;
        t_owns = info.writeOwnerId == self;
        for (int i = 0; i < info.lockCount; i++) {
            if (t_owns)
                sceKernelUnlockWriteRWLock(rwlock);
            else
                sceKernelUnlockReadRWLock(rwlock);
        }
    }
    sceKernelDeleteCallback(callback);
    return 0;
}

static SceUID start_t(void) {
    const SceUID t = sceKernelCreateThread("T", t_entry, THREAD_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    return t;
}

// Resets what the threads record and creates the gate and the lock of the case
static void begin(Lock waits, Lock takes, int is_pending) {
    t_lock = waits;
    callback_take = takes;
    pending = is_pending;
    callback = 0;
    callback_lock = t_wait = t_count = t_owns = NOT_DONE;
    callback_done = 0;
    saw_wait = saw_gate = 0;
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    if (waits == MUTEX)
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
    else
        rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
}

// Prints what every case prints and deletes the objects
static void finish(const char *name) {
    PRINTF(callback_lock, "%s: the callback's lock", name);
    PRINTF(t_wait, "%s: T's wait", name);
    PRINTF(t_count, "%s: the lock count after T's wait", name);
    PRINTF(t_owns, "%s: T owns the write lock or the mutex after T's wait", name);
    sceKernelDeleteSema(gate);
    if (t_lock == MUTEX)
        sceKernelDeleteMutex(mutex);
    else
        sceKernelDeleteRWLock(rwlock);
}

// The main thread holds the write lock while T waits, notifies T's callback, then unlocks the
// write lock and lets the callback take its lock
static void notified_case(const char *name, Lock waits, Lock takes) {
    begin(waits, takes, 0);
    sceKernelLockWriteRWLock(rwlock, NULL);
    const SceUInt32 type = WAIT_TYPE_RW_LOCK | WAIT_TYPE_CB;
    const SceUID t = start_t();
    TEST_WAIT_FOR(test_in_wait(t, type, rwlock));
    saw_wait = test_in_wait(t, type, rwlock);
    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, gate));
    saw_gate = test_in_wait(t, WAIT_TYPE_SEMA, gate);
    sceKernelUnlockWriteRWLock(rwlock);
    sceKernelSignalSema(gate, 1);
    // The callback's return and T's wait on the rwlock again are separate steps, so this waits for
    // both, or for T's wait to end
    TEST_WAIT_FOR(t_wait != NOT_DONE || (callback_done && test_in_wait(t, type, rwlock)));
    const int waits_again = t_wait == NOT_DONE && callback_done && test_in_wait(t, type, rwlock);
    test_join_thread(t);
    PRINTF(saw_wait, "%s: T waits on the rwlock before the notify", name);
    PRINTF(saw_gate, "%s: T waits on the gate after the notify", name);
    PRINTF(waits_again, "%s: T waits on the rwlock again after its callback", name);
    finish(name);
}

// T's callback is pending when T starts its wait on the free lock, so the callback takes it first
static void pending_case(const char *name, Lock lock) {
    begin(lock, lock, 1);
    test_join_thread(start_t());
    finish(name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    notified_case("writer whose callback takes the write lock", WRITE_LOCK, WRITE_LOCK);
    notified_case("writer whose callback takes a read lock", WRITE_LOCK, READ_LOCK);
    notified_case("reader whose callback takes the write lock", READ_LOCK, WRITE_LOCK);
    notified_case("reader whose callback takes a read lock", READ_LOCK, READ_LOCK);
    pending_case("mutex with the callback pending", MUTEX);
    pending_case("rwlock writer with the callback pending", WRITE_LOCK);

    test_exit();
    return 0;
}
