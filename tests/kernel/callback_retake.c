// What a wait with callbacks returns, and what state its lock is in, when the callback it runs
// changes the lock. A thread T waits with callbacks on a lock. A notified callback takes T out of
// the lock's queue while it runs. In the meantime the lock changes, either the main thread unlocks
// it or takes it in another mode, or the callback itself takes it. Once the callback returns, T's
// wait either takes the lock or waits on it again, and the cases show which: for a mutex or a
// lightweight mutex that T itself now owns, recursive or not, and for a rwlock that a reader holds,
// with T coming back as a reader or as a writer.
//
// Every thread is pinned to core 0. The main thread runs at 0x60 and the other threads at 0xB0. T
// creates its callback in its own thread before it waits. The callback blocks in a wait on a gate
// semaphore, so the main thread changes the lock while T is out of the lock's queue. Every wait has
// a timeout of 8 s, so a wrong result can't hang the test, except T's wait in the mutex cases,
// which has 1 s, far longer than the callback takes. Every case has fresh objects and fresh
// threads. The main thread waits until T is in the lock's wait, notifies the callback, waits until
// T is in the gate wait, changes the lock, signals the gate and joins the threads. What the threads
// record is printed after they ended.
//
// The cases:
// - recursive mutex the callback locks: the main thread locks the mutex once and T waits with
//   sceKernelLockMutexCB. The main thread unlocks it, so nobody is queued and it is free. Then it
//   signals the gate. The callback locks the mutex itself and returns while it holds it. The main
//   thread waits until the callback returned and T waits on the mutex again, or T's wait ended.
//   Once T's wait returns, T reads the mutex's info. Then T unlocks the mutex as often as the count
//   says.
// - mutex the callback locks: the same with a mutex that isn't recursive
// - recursive lightweight mutex the callback locks: the same with a lightweight mutex that is
//   recursive, waited on with sceKernelLockLwMutexCB
// - lightweight mutex the callback locks: the same with a lightweight mutex that isn't recursive
// - reader comes back to a read lock: the main thread holds the rwlock's write lock and T waits
//   with sceKernelLockReadRWLockCB. While T's callback waits, the main thread unlocks the write
//   lock, takes a read lock and signals the gate. The callback returns at once. After T's wait, T
//   reads the rwlock's info itself, unlocks its read lock if it got it, and the main thread unlocks
//   its own read lock.
// - writer comes back to a read lock: the same, but T waits for the write lock with
//   sceKernelLockWriteRWLockCB, so after its callback the main thread's read lock is in its way.
//   The main thread waits until T waits again, then unlocks its read lock.
// - reader behind a writer that leaves for callbacks: the main thread holds a read lock. W waits
//   for the write lock with sceKernelLockWriteRWLockCB, and R waits for a read lock without
//   callbacks, queued behind W. While W's callback waits on the gate, the main thread checks
//   whether R got its read lock, though the main thread still holds one. Then it unlocks its read
//   lock and signals the gate.
//
// Each of the four mutex cases prints:
// - whether T waits on the lock with the callback bit before the notify
// - whether T waits on the gate after the notify
// - the result of the callback's own lock
// - whether T waits on the lock with the callback bit again after its callback
// - the result of T's wait on the lock
// - the lock's count right after T's wait
// - whether T owns the lock right after T's wait
// The reader case prints the first two values, T's wait result and the rwlock's lock count right
// after T's wait. The writer case prints the first two values, whether T waits on the rwlock again
// after its callback, and T's wait result. The last case prints:
// - whether W waits on the rwlock with the callback bit before the notify
// - whether R waits on the rwlock before the notify
// - whether W waits on the gate after the notify
// - whether R got its read lock while W's callback waits
// - R's wait result
// - W's wait result
//
// On a PS Vita TV on firmware 3.65 a mutex or lightweight mutex that T's callback locked keeps T's
// outer wait waiting, on a lock T owns itself, recursive or not: T waits on it again after its
// callback until the outer timeout (0x80028005), and the count stays the callback's 1. The
// returning reader joins the main thread's read lock at once with a count of 2, the returning
// writer waits again until the read unlock, and R gets its read lock while W runs its callback.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define THREAD_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
// Longer than the 5 s the main thread waits for anything, so a waiter is still waiting then
#define LONG_TIMEOUT (4 * TEST_WAIT_US)
// The outer wait of the mutex cases, far longer than the callback takes
#define MUTEX_TIMEOUT 1000000

typedef enum Kind { MUTEX, LW_MUTEX, READ_LOCK, WRITE_LOCK } Kind;

static Kind kind;
static SceUID mutex, rwlock, gate;
static SceKernelLwMutexWork lw_mutex;
// The UID that T's wait on the lock reports
static SceUID lock_id;
static volatile SceUID callback;
static volatile int callback_lock, callback_done, t_wait, t_count, t_owns, r_wait, r_done;
static int saw_wait, saw_gate;

// Blocks on the gate, then locks the mutex of the case, if it has one, and returns holding it
static int retake_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = LONG_TIMEOUT;
    sceKernelWaitSema(gate, 1, &timeout);
    timeout = LONG_TIMEOUT;
    if (kind == MUTEX)
        callback_lock = sceKernelLockMutex(mutex, 1, &timeout);
    else if (kind == LW_MUTEX)
        callback_lock = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
    callback_done = 1;
    return 0;
}

// Creates its callback, waits on the lock with callbacks, reads the lock's state and gives the
// lock back. T and W of the last case use it.
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const SceUID self = sceKernelGetThreadId();
    callback = sceKernelCreateCallback("retake", 0, retake_callback, NULL);
    SceUInt timeout = kind == MUTEX || kind == LW_MUTEX ? MUTEX_TIMEOUT : LONG_TIMEOUT;
    switch (kind) {
    case MUTEX: {
        t_wait = sceKernelLockMutexCB(mutex, 1, &timeout);
        MutexInfo info = { .size = sizeof(info) };
        sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
        t_count = info.currentCount;
        t_owns = info.currentOwnerId == self;
        if (info.currentOwnerId == self) {
            for (int i = 0; i < info.currentCount; i++)
                sceKernelUnlockMutex(mutex, 1);
        }
        break;
    }
    case LW_MUTEX: {
        t_wait = sceKernelLockLwMutexCB(&lw_mutex, 1, &timeout);
        LwMutexInfo info = { .size = sizeof(info) };
        sceKernelGetLwMutexInfo(&lw_mutex, &info);
        t_count = info.currentCount;
        t_owns = info.currentOwnerId == self;
        if (info.currentOwnerId == self) {
            for (int i = 0; i < info.currentCount; i++)
                sceKernelUnlockLwMutex(&lw_mutex, 1);
        }
        break;
    }
    case READ_LOCK: {
        t_wait = sceKernelLockReadRWLockCB(rwlock, &timeout);
        SceKernelRWLockInfo info = { .size = sizeof(info) };
        sceKernelGetRWLockInfo(rwlock, &info);
        t_count = info.lockCount;
        if (t_wait == 0)
            sceKernelUnlockReadRWLock(rwlock);
        break;
    }
    case WRITE_LOCK:
        t_wait = sceKernelLockWriteRWLockCB(rwlock, &timeout);
        if (t_wait == 0)
            sceKernelUnlockWriteRWLock(rwlock);
        break;
    }
    sceKernelDeleteCallback(callback);
    return 0;
}

// Takes a read lock without callbacks and gives it back at once
static int r_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = LONG_TIMEOUT;
    r_wait = sceKernelLockReadRWLock(rwlock, &timeout);
    r_done = 1;
    if (r_wait == 0)
        sceKernelUnlockReadRWLock(rwlock);
    return 0;
}

static SceUID start_thread(SceKernelThreadEntry entry) {
    const SceUID thread = sceKernelCreateThread("thread", entry, THREAD_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    return thread;
}

static SceUID lw_mutex_uid(void) {
    LwMutexInfo info = { .size = sizeof(info) };
    return sceKernelGetLwMutexInfo(&lw_mutex, &info) == 0 ? info.uid : -1;
}

// Resets what the threads record and creates the gate
static void begin(Kind k) {
    kind = k;
    callback = 0;
    callback_lock = t_wait = t_count = t_owns = r_wait = NOT_DONE;
    callback_done = r_done = 0;
    saw_wait = saw_gate = 0;
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
}

static void finish(void) {
    sceKernelDeleteSema(gate);
    if (kind == MUTEX)
        sceKernelDeleteMutex(mutex);
    else if (kind == LW_MUTEX)
        sceKernelDeleteLwMutex(&lw_mutex);
    else
        sceKernelDeleteRWLock(rwlock);
}

// Starts a thread that waits on the lock with callbacks, and waits until it does
static SceUID start_t(SceUInt32 wait_type) {
    const SceUID t = start_thread(t_entry);
    TEST_WAIT_FOR(test_in_wait(t, wait_type | WAIT_TYPE_CB, lock_id));
    saw_wait = test_in_wait(t, wait_type | WAIT_TYPE_CB, lock_id);
    return t;
}

// Notifies the callback of the waiting thread, and waits until the callback waits on the gate
static void notify_t(SceUID t) {
    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, gate));
    saw_gate = test_in_wait(t, WAIT_TYPE_SEMA, gate);
}

static void print_start(const char *name) {
    PRINTF(saw_wait, "%s: T waits on the lock before the notify", name);
    PRINTF(saw_gate, "%s: T waits on the gate after the notify", name);
}

// A mutex or a lightweight mutex that the main thread holds, and that the callback locks once the
// main thread unlocked it
static void mutex_case(const char *name, Kind k, int recursive) {
    begin(k);
    if (k == MUTEX) {
        mutex = sceKernelCreateMutex("mutex", recursive ? SCE_KERNEL_MUTEX_ATTR_RECURSIVE : 0, 0, NULL);
        lock_id = mutex;
        sceKernelLockMutex(mutex, 1, NULL);
    } else {
        sceKernelCreateLwMutex(&lw_mutex, "lw mutex", recursive ? LW_MUTEX_ATTR_RECURSIVE : 0, 0, NULL);
        lock_id = lw_mutex_uid();
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    }
    const SceUInt32 type = (k == MUTEX ? WAIT_TYPE_MUTEX : WAIT_TYPE_LW_MUTEX) | WAIT_TYPE_CB;
    const SceUID t = start_t(k == MUTEX ? WAIT_TYPE_MUTEX : WAIT_TYPE_LW_MUTEX);
    notify_t(t);
    if (k == MUTEX)
        sceKernelUnlockMutex(mutex, 1);
    else
        sceKernelUnlockLwMutex(&lw_mutex, 1);
    sceKernelSignalSema(gate, 1);
    // The callback's return and T's wait on the lock again are separate steps, so this waits for
    // both, or for T's wait to end
    TEST_WAIT_FOR(t_wait != NOT_DONE || (callback_done && test_in_wait(t, type, lock_id)));
    const int waits_again = t_wait == NOT_DONE && callback_done && test_in_wait(t, type, lock_id);
    test_join_thread(t);
    print_start(name);
    PRINTF(callback_lock, "%s: the callback's lock", name);
    PRINTF(waits_again, "%s: T waits on the lock again after its callback", name);
    PRINTF(t_wait, "%s: T's wait", name);
    PRINTF(t_count, "%s: the lock's count after T's wait", name);
    PRINTF(t_owns, "%s: T owns the lock after T's wait", name);
    finish();
}

// The main thread unlocks the write lock, takes a read lock, and T's wait for a read lock gets
// back to it
static void reader_case(const char *name) {
    begin(READ_LOCK);
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    lock_id = rwlock;
    sceKernelLockWriteRWLock(rwlock, NULL);
    const SceUID t = start_t(WAIT_TYPE_RW_LOCK);
    notify_t(t);
    sceKernelUnlockWriteRWLock(rwlock);
    SceUInt timeout = LONG_TIMEOUT;
    sceKernelLockReadRWLock(rwlock, &timeout);
    sceKernelSignalSema(gate, 1);
    test_join_thread(t);
    sceKernelUnlockReadRWLock(rwlock);
    print_start(name);
    PRINTF(t_wait, "%s: T's wait", name);
    PRINTF(t_count, "%s: the lock count after T's wait", name);
    finish();
}

// The same, but T's wait is for the write lock, which the main thread's read lock holds back
static void writer_case(const char *name) {
    begin(WRITE_LOCK);
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    lock_id = rwlock;
    sceKernelLockWriteRWLock(rwlock, NULL);
    const SceUID t = start_t(WAIT_TYPE_RW_LOCK);
    notify_t(t);
    sceKernelUnlockWriteRWLock(rwlock);
    SceUInt timeout = LONG_TIMEOUT;
    sceKernelLockReadRWLock(rwlock, &timeout);
    sceKernelSignalSema(gate, 1);
    const SceUInt32 type = WAIT_TYPE_RW_LOCK | WAIT_TYPE_CB;
    TEST_WAIT_FOR(t_wait != NOT_DONE || test_in_wait(t, type, rwlock));
    const int waits_again = test_in_wait(t, type, rwlock);
    sceKernelUnlockReadRWLock(rwlock);
    test_join_thread(t);
    print_start(name);
    PRINTF(waits_again, "%s: T waits on the rwlock again after its callback", name);
    PRINTF(t_wait, "%s: T's wait", name);
    finish();
}

// A reader queued behind a writer whose callback runs while the main thread holds a read lock
static void behind_writer_case(const char *name) {
    begin(WRITE_LOCK);
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    lock_id = rwlock;
    sceKernelLockReadRWLock(rwlock, NULL);
    const SceUID w = start_t(WAIT_TYPE_RW_LOCK);
    const SceUID r = start_thread(r_entry);
    TEST_WAIT_FOR(r_done || test_in_wait(r, WAIT_TYPE_RW_LOCK, rwlock));
    const int r_waits = test_in_wait(r, WAIT_TYPE_RW_LOCK, rwlock);
    notify_t(w);
    // If W's departure doesn't let R in, this waits the full 5 s
    TEST_WAIT_FOR(r_done);
    const int r_got = r_done;
    sceKernelUnlockReadRWLock(rwlock);
    sceKernelSignalSema(gate, 1);
    test_join_thread(w);
    test_join_thread(r);
    PRINTF(saw_wait, "%s: W waits on the rwlock before the notify", name);
    PRINTF(r_waits, "%s: R waits on the rwlock before the notify", name);
    PRINTF(saw_gate, "%s: W waits on the gate after the notify", name);
    PRINTF(r_got, "%s: R got its read lock while W's callback waits", name);
    PRINTF(r_wait, "%s: R's wait", name);
    PRINTF(t_wait, "%s: W's wait", name);
    finish();
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    mutex_case("recursive mutex the callback locks", MUTEX, 1);
    mutex_case("mutex the callback locks", MUTEX, 0);
    mutex_case("recursive lightweight mutex the callback locks", LW_MUTEX, 1);
    mutex_case("lightweight mutex the callback locks", LW_MUTEX, 0);
    reader_case("reader comes back to a read lock");
    writer_case("writer comes back to a read lock");
    behind_writer_case("reader behind a writer that leaves for callbacks");

    test_exit();
    return 0;
}
