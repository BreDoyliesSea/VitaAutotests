// The lock count, write owner and waiter counts a reader/writer lock reports after each step: read
// locks held more than once and by several threads, a holder that waits for the other kind of lock,
// write locks taken again with and without the recursive attribute, and unlocks by threads that
// don't hold the lock. Waits that could deadlock all have a short timeout.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID rwlock, release;
static SceUID main_thread, holder;
static volatile int holder_result, holder_unlock, other_result;

// Prints what the lock's info shows. The write owner prints as 0 for none, 1 for this thread and
// 2 for another thread.
static void print_state(const char *step) {
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    const int result = sceKernelGetRWLockInfo(rwlock, &info);
    if (result < 0) {
        PRINTF(result, "%s: info", step);
        return;
    }
    const int owner = info.writeOwnerId == 0 ? 0 : info.writeOwnerId == main_thread ? 1 : 2;
    PRINTF(info.lockCount, "%s: lock count", step);
    PRINTF(owner, "%s: write owner", step);
    PRINTF(info.numReadWaitThreads, "%s: waiting readers", step);
    PRINTF(info.numWriteWaitThreads, "%s: waiting writers", step);
}

// Takes a read lock and keeps it until release is signaled
static int read_holder(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    holder_result = sceKernelLockReadRWLock(rwlock, &timeout);
    timeout = TEST_WAIT_US;
    sceKernelWaitSema(release, 1, &timeout);
    holder_unlock = sceKernelUnlockReadRWLock(rwlock);
    return 0;
}

// Takes the write lock and keeps it until release is signaled
static int write_holder(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    holder_result = sceKernelLockWriteRWLock(rwlock, &timeout);
    timeout = TEST_WAIT_US;
    sceKernelWaitSema(release, 1, &timeout);
    holder_unlock = sceKernelUnlockWriteRWLock(rwlock);
    return 0;
}

static int read_unlock_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    other_result = sceKernelUnlockReadRWLock(rwlock);
    return 0;
}

static int try_read_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    other_result = sceKernelTryLockReadRWLock(rwlock);
    if (other_result == 0)
        sceKernelUnlockReadRWLock(rwlock);
    return 0;
}

static int try_write_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    other_result = sceKernelTryLockWriteRWLock(rwlock);
    if (other_result == 0)
        sceKernelUnlockWriteRWLock(rwlock);
    return 0;
}

// Waits for a read lock or the write lock, depending on arg, and unlocks it again
static int waiter(SceSize args, void *argp) {
    (void)args;
    const int write = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    if (write) {
        if (sceKernelLockWriteRWLock(rwlock, &timeout) == 0)
            sceKernelUnlockWriteRWLock(rwlock);
    } else {
        if (sceKernelLockReadRWLock(rwlock, &timeout) == 0)
            sceKernelUnlockReadRWLock(rwlock);
    }
    return 0;
}

// Runs entry on another thread and returns the result it stored
static int on_other_thread(SceKernelThreadEntry entry) {
    other_result = 0xDEAD;
    test_join_thread(test_start_thread(entry));
    return other_result;
}

static void start_holder(SceKernelThreadEntry entry) {
    holder_result = holder_unlock = 0xDEAD;
    holder = test_start_thread(entry);
    test_wait_until_waiting(holder);
}

static void end_holder(void) {
    sceKernelSignalSema(release, 1);
    test_join_thread(holder);
}

int main(void) {
    main_thread = sceKernelGetThreadId();
    release = sceKernelCreateSema("release", 0, 0, 1, NULL);
    SceUInt timeout;

    // Read locks of one thread and of two
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    print_state("created");
    sceKernelLockReadRWLock(rwlock, NULL);
    print_state("read locked once");
    sceKernelLockReadRWLock(rwlock, NULL);
    print_state("read locked twice");
    start_holder(read_holder);
    PRINT("another thread read locks", holder_result);
    print_state("read locked twice here and once there");
    PRINT("a read unlock by a thread with no read lock", on_other_thread(read_unlock_thread));
    print_state("after it");
    PRINT("read unlock here", sceKernelUnlockReadRWLock(rwlock));
    PRINT("read unlock here again", sceKernelUnlockReadRWLock(rwlock));
    end_holder();
    PRINT("the other thread's read unlock", holder_unlock);
    print_state("all unlocked");
    PRINT("one read unlock too many", sceKernelUnlockReadRWLock(rwlock));

    // A reader that waits for the write lock, which only it could give up
    sceKernelLockReadRWLock(rwlock, NULL);
    timeout = 100000;
    PRINT("write lock while holding a read lock, with a timeout", sceKernelLockWriteRWLock(rwlock, &timeout));
    print_state("after that wait");
    PRINT("try read lock on another thread after it", on_other_thread(try_read_thread));
    sceKernelUnlockReadRWLock(rwlock);

    // A writer that waits for a read lock, or for the write lock again
    sceKernelLockWriteRWLock(rwlock, NULL);
    print_state("write locked");
    timeout = 100000;
    PRINT("read lock while holding the write lock, with a timeout", sceKernelLockReadRWLock(rwlock, &timeout));
    timeout = 100000;
    PRINT("write lock again, with a timeout", sceKernelLockWriteRWLock(rwlock, &timeout));
    print_state("after those waits");
    PRINT("a read unlock by another thread while write locked", on_other_thread(read_unlock_thread));
    print_state("after it");
    PRINT("write unlock", sceKernelUnlockWriteRWLock(rwlock));
    print_state("write unlocked");

    // Waiters of each kind behind another thread's write lock
    start_holder(write_holder);
    PRINT("another thread write locks", holder_result);
    print_state("write locked by another thread");
    const int read = 0, write = 1;
    const SceUID reader = test_start_thread_arg(waiter, read);
    test_wait_until_waiting(reader);
    const SceUID writer = test_start_thread_arg(waiter, write);
    test_wait_until_waiting(writer);
    print_state("with a reader and a writer waiting");
    end_holder();
    test_join_thread(reader);
    test_join_thread(writer);
    PRINT("the writer's unlock", holder_unlock);
    print_state("after the waiters are done");
    sceKernelDeleteRWLock(rwlock);

    // The recursive attribute
    rwlock = sceKernelCreateRWLock("recursive", RW_LOCK_ATTR_RECURSIVE, NULL);
    PRINT("recursive: write lock", sceKernelTryLockWriteRWLock(rwlock));
    timeout = 100000;
    PRINT("recursive: write lock again", sceKernelLockWriteRWLock(rwlock, &timeout));
    PRINT("recursive: try write lock a third time", sceKernelTryLockWriteRWLock(rwlock));
    print_state("recursive: after three write locks");
    PRINT("recursive: try read lock while holding the write lock", sceKernelTryLockReadRWLock(rwlock));
    print_state("recursive: after it");
    PRINT("recursive: try write lock on another thread", on_other_thread(try_write_thread));
    PRINT("recursive: try read lock on another thread", on_other_thread(try_read_thread));
    PRINT("recursive: write unlock", sceKernelUnlockWriteRWLock(rwlock));
    print_state("recursive: after one write unlock");
    PRINT("recursive: try write lock on another thread after it", on_other_thread(try_write_thread));
    // Unlocks until it fails, at most five times
    for (int i = 0; i < 5; i++) {
        const int result = sceKernelUnlockWriteRWLock(rwlock);
        PRINTF(result, "recursive: write unlock %d more", i + 1);
        if (result < 0)
            break;
    }
    print_state("recursive: unlocked");
    // Any read lock the try read lock above took
    sceKernelUnlockReadRWLock(rwlock);
    print_state("recursive: after a read unlock");
    PRINT("recursive: read lock", sceKernelTryLockReadRWLock(rwlock));
    PRINT("recursive: try write lock while holding a read lock", sceKernelTryLockWriteRWLock(rwlock));
    print_state("recursive: after it");
    sceKernelUnlockReadRWLock(rwlock);
    PRINT("recursive: try write lock on another thread at the end", on_other_thread(try_write_thread));
    sceKernelDeleteRWLock(rwlock);
    sceKernelDeleteSema(release);

    test_exit();
    return 0;
}
