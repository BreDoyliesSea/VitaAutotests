// What happens to a lock whose owner thread ends while another thread waits for it: a recursive
// mutex, a mutex whose owner exits and deletes itself, a lightweight mutex, and both locks of a
// reader/writer lock
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { MUTEX_RECURSIVE, MUTEX_EXIT_DELETE, LW_MUTEX, WRITE_LOCK, READ_LOCK };

static SceUID mutex, rwlock, go;
static SceKernelLwMutexWork lw_mutex;
static volatile int kind, waiter_result;

// Takes the lock, waits for go, and ends without giving it back
static int owner_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    switch (kind) {
    case MUTEX_RECURSIVE: sceKernelLockMutex(mutex, 3, NULL); break;
    case MUTEX_EXIT_DELETE: sceKernelLockMutex(mutex, 1, NULL); break;
    case LW_MUTEX: sceKernelLockLwMutex(&lw_mutex, 1, NULL); break;
    case WRITE_LOCK: sceKernelLockWriteRWLock(rwlock, NULL); break;
    case READ_LOCK: sceKernelLockReadRWLock(rwlock, NULL); break;
    }
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(go, 1, &timeout);
    if (kind == MUTEX_EXIT_DELETE)
        sceKernelExitDeleteThread(0);
    return 0;
}

// Waits for the lock for a second, far longer than the owner takes to end once told to
static int waiter_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 1000000;
    switch (kind) {
    case MUTEX_RECURSIVE:
    case MUTEX_EXIT_DELETE:
        waiter_result = sceKernelLockMutex(mutex, 1, &timeout);
        if (waiter_result == 0)
            sceKernelUnlockMutex(mutex, 1);
        break;
    case LW_MUTEX:
        waiter_result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        if (waiter_result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case WRITE_LOCK:
    case READ_LOCK:
        waiter_result = sceKernelLockWriteRWLock(rwlock, &timeout);
        if (waiter_result == 0)
            sceKernelUnlockWriteRWLock(rwlock);
        break;
    }
    return 0;
}

static void run(const char *name, int new_kind) {
    kind = new_kind;
    waiter_result = 0xDEAD;
    const SceUID owner = test_start_thread(owner_thread);
    test_wait_until_waiting(owner);
    const SceUID waiter = test_start_thread(waiter_thread);
    test_wait_until_waiting(waiter);
    sceKernelSignalSema(go, 1);
    test_join_thread(waiter);
    if (new_kind != MUTEX_EXIT_DELETE)
        test_join_thread(owner);
    PRINTF(waiter_result, "%s: the waiter's lock", name);
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);

    mutex = sceKernelCreateMutex("recursive", MUTEX_ATTR_RECURSIVE, 0, NULL);
    run("recursive mutex locked 3 times", MUTEX_RECURSIVE);
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    PRINT("recursive mutex locked 3 times: its count after", info.currentCount);
    PRINT("recursive mutex locked 3 times: it has an owner after", info.currentOwnerId != 0);
    sceKernelDeleteMutex(mutex);

    mutex = sceKernelCreateMutex("exit delete", 0, 0, NULL);
    run("mutex whose owner exits and deletes itself", MUTEX_EXIT_DELETE);
    sceKernelDeleteMutex(mutex);

    sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
    run("lightweight mutex", LW_MUTEX);
    LwMutexInfo lw_info = { .size = sizeof(lw_info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &lw_info);
    PRINT("lightweight mutex: its count after", lw_info.currentCount);
    PRINT("lightweight mutex: try to lock it", sceKernelTryLockLwMutex(&lw_mutex, 1));
    sceKernelDeleteLwMutex(&lw_mutex);

    rwlock = sceKernelCreateRWLock("write", 0, NULL);
    run("write lock", WRITE_LOCK);
    PRINT("write lock: try to lock it for writing after", sceKernelTryLockWriteRWLock(rwlock));
    sceKernelDeleteRWLock(rwlock);

    rwlock = sceKernelCreateRWLock("read", 0, NULL);
    run("read lock", READ_LOCK);
    PRINT("read lock: try to lock it for writing after", sceKernelTryLockWriteRWLock(rwlock));
    sceKernelDeleteRWLock(rwlock);

    test_exit();
    return 0;
}
