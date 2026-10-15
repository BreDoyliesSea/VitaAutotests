// More of what happens to a lock whose owner thread ends, past the cases of owner_end. An owner
// thread takes the lock, waits for go and ends, by returning from its entry unless the case says
// otherwise. The test prints:
// - a mutex whose owner ends with no waiter: its count and whether it has an owner after, and a
//   try lock
// - a mutex whose owner ends with sceKernelExitThread while a thread waits: the waiter's lock
// - a write lock whose owner ends with no waiter: its count and whether it has a write owner
//   after, and a try write lock
// - a write lock its owner holds twice while a writer waits: the waiter's lock, and the count
//   while the waiter holds it
// - a write lock whose owner exits and deletes itself while a writer waits: the waiter's lock
// - a write lock whose owner ends while a reader waits: the reader's lock
// - a read lock whose owner ends with no waiter: its count after, and a try write lock
// - a lightweight mutex whose owner exits and deletes itself while a thread waits: the waiter's
//   lock, the count after, and whether it still reports the ended thread as its owner
//
// On a PS Vita TV on firmware 3.65 a mutex and a write lock are released when their owner ends,
// with a waiter or without one, and however the owner ends. A write lock held twice is released
// whole and its waiter gets it with a count of 1, and a waiting reader gets a released write
// lock too. A read lock stays held, with a count of 1 and no write owner, and a lightweight
// mutex whose owner exits and deletes itself stays locked, still owned by the ended thread.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind {
    MUTEX_NO_WAITER,
    MUTEX_EXIT_THREAD,
    WRITE_NO_WAITER,
    WRITE_TWICE,
    WRITE_EXIT_DELETE,
    WRITE_READER_WAITS,
    READ_NO_WAITER,
    LW_EXIT_DELETE,
};

static SceUID mutex, rwlock, go;
static SceKernelLwMutexWork lw_mutex;
static volatile int kind, waiter_result, waiter_count;

// Takes the lock, waits for go, and ends the way the case says without giving it back
static int owner_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    switch (kind) {
    case MUTEX_NO_WAITER:
    case MUTEX_EXIT_THREAD: sceKernelLockMutex(mutex, 1, NULL); break;
    case WRITE_NO_WAITER:
    case WRITE_EXIT_DELETE:
    case WRITE_READER_WAITS: sceKernelLockWriteRWLock(rwlock, NULL); break;
    case WRITE_TWICE:
        sceKernelLockWriteRWLock(rwlock, NULL);
        sceKernelLockWriteRWLock(rwlock, NULL);
        break;
    case READ_NO_WAITER: sceKernelLockReadRWLock(rwlock, NULL); break;
    case LW_EXIT_DELETE: sceKernelLockLwMutex(&lw_mutex, 1, NULL); break;
    }
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(go, 1, &timeout);
    if (kind == MUTEX_EXIT_THREAD)
        sceKernelExitThread(0);
    if (kind == WRITE_EXIT_DELETE || kind == LW_EXIT_DELETE)
        sceKernelExitDeleteThread(0);
    return 0;
}

// Waits for the lock for a second, far longer than the owner takes to end once told to
static int waiter_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 1000000;
    switch (kind) {
    case MUTEX_EXIT_THREAD:
        waiter_result = sceKernelLockMutex(mutex, 1, &timeout);
        if (waiter_result == 0)
            sceKernelUnlockMutex(mutex, 1);
        break;
    case WRITE_TWICE:
    case WRITE_EXIT_DELETE:
        waiter_result = sceKernelLockWriteRWLock(rwlock, &timeout);
        if (waiter_result == 0) {
            SceKernelRWLockInfo info = { .size = sizeof(info) };
            sceKernelGetRWLockInfo(rwlock, &info);
            waiter_count = info.lockCount;
            sceKernelUnlockWriteRWLock(rwlock);
        }
        break;
    case WRITE_READER_WAITS:
        waiter_result = sceKernelLockReadRWLock(rwlock, &timeout);
        if (waiter_result == 0)
            sceKernelUnlockReadRWLock(rwlock);
        break;
    case LW_EXIT_DELETE:
        waiter_result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        if (waiter_result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    }
    return 0;
}

// Runs one case, with a waiter if it has one, and returns the owner's UID. An owner that doesn't
// delete itself is left for the caller to delete after its checks, so they see what its end did
// rather than its delete.
static SceUID run(int new_kind, int with_waiter) {
    kind = new_kind;
    waiter_result = 0xDEAD;
    waiter_count = 0xDEAD;
    const SceUID owner = test_start_thread(owner_thread);
    test_wait_until_waiting(owner);
    SceUID waiter = -1;
    if (with_waiter) {
        waiter = test_start_thread(waiter_thread);
        test_wait_until_waiting(waiter);
    }
    sceKernelSignalSema(go, 1);
    if (with_waiter)
        test_join_thread(waiter);
    // An owner that exits and deletes itself can't be waited for, the waiter's wait outlasts it
    if (new_kind != WRITE_EXIT_DELETE && new_kind != LW_EXIT_DELETE)
        sceKernelWaitThreadEnd(owner, NULL, NULL);
    return owner;
}

static void print_mutex(const char *name) {
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    PRINTF(info.currentCount, "%s: its count after", name);
    PRINTF(info.currentOwnerId != 0, "%s: it has an owner after", name);
}

static void print_rwlock(const char *name) {
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    sceKernelGetRWLockInfo(rwlock, &info);
    PRINTF(info.lockCount, "%s: its count after", name);
    PRINTF(info.writeOwnerId != 0, "%s: it has a write owner after", name);
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);

    mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
    SceUID owner = run(MUTEX_NO_WAITER, 0);
    print_mutex("mutex, no waiter");
    const int locked = sceKernelTryLockMutex(mutex, 1);
    PRINT("mutex, no waiter: try to lock it", locked);
    if (locked == 0)
        sceKernelUnlockMutex(mutex, 1);
    sceKernelDeleteThread(owner);
    sceKernelDeleteMutex(mutex);

    mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
    owner = run(MUTEX_EXIT_THREAD, 1);
    PRINT("mutex whose owner calls sceKernelExitThread: the waiter's lock", waiter_result);
    sceKernelDeleteThread(owner);
    sceKernelDeleteMutex(mutex);

    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    owner = run(WRITE_NO_WAITER, 0);
    print_rwlock("write lock, no waiter");
    PRINT("write lock, no waiter: try to lock it for writing", sceKernelTryLockWriteRWLock(rwlock));
    sceKernelDeleteThread(owner);
    sceKernelDeleteRWLock(rwlock);

    rwlock = sceKernelCreateRWLock("rwlock", RW_LOCK_ATTR_RECURSIVE, NULL);
    owner = run(WRITE_TWICE, 1);
    PRINT("write lock held twice: the waiter's lock", waiter_result);
    PRINT("write lock held twice: the count while the waiter holds it", waiter_count);
    sceKernelDeleteThread(owner);
    sceKernelDeleteRWLock(rwlock);

    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    run(WRITE_EXIT_DELETE, 1);
    PRINT("write lock whose owner exits and deletes itself: the waiter's lock", waiter_result);
    sceKernelDeleteRWLock(rwlock);

    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    owner = run(WRITE_READER_WAITS, 1);
    PRINT("write lock, a reader waits: the reader's lock", waiter_result);
    sceKernelDeleteThread(owner);
    sceKernelDeleteRWLock(rwlock);

    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    owner = run(READ_NO_WAITER, 0);
    print_rwlock("read lock, no waiter");
    PRINT("read lock, no waiter: try to lock it for writing", sceKernelTryLockWriteRWLock(rwlock));
    sceKernelDeleteThread(owner);
    sceKernelDeleteRWLock(rwlock);

    sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
    owner = run(LW_EXIT_DELETE, 1);
    PRINT("lightweight mutex whose owner exits and deletes itself: the waiter's lock", waiter_result);
    LwMutexInfo lw_info = { .size = sizeof(lw_info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &lw_info);
    PRINT("lightweight mutex whose owner exits and deletes itself: its count after", lw_info.currentCount);
    PRINT("lightweight mutex whose owner exits and deletes itself: its owner is the ended thread", lw_info.currentOwnerId == owner);
    sceKernelDeleteLwMutex(&lw_mutex);

    sceKernelDeleteSema(go);
    test_exit();
    return 0;
}
