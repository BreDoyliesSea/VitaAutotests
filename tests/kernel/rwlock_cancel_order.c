// What sceKernelCancelRWLock does with the lock and its flag. The wake order of the waiters is in
// rwlock_cancel_wake_order. The cases:
// - Read holds: two threads hold read locks, then the main thread cancels with flag 0. It prints
//   the counts the cancel wrote, the lock count after it, whether the main thread can write lock
//   at once, and what the two read unlocks of the holders return afterwards.
// - Flag 1: the cancel makes the caller the write owner. It prints the lock count and whether the
//   caller owns the lock after it, with the lock free, with another thread holding a read lock,
//   and with the caller holding a recursive write lock twice.
// - Bad flags, last: flags 2, -1 and 0x100 on a lock the main thread write locks while a reader
//   waits, which must change nothing. Then flag 2 on an unknown UID and on a semaphore, to see
//   which error comes first.
//
// On a PS Vita TV on firmware 3.65 a cancel with flag 0 forgets the read holds. The lock count is
// 0, the main thread can write lock at once, and each holder's read unlock fails with 0x800281E5.
// Flag 1 makes the caller the write owner with a count of 1, even while another thread reads, whose
// read unlock then fails with 0x800281E7, and when the caller held the lock twice. Flags 2, -1 and
// 0x100 fail with 0x8002000A and change nothing, but the count pointers are still written. With
// flag 2 an unknown UID fails with 0x800281E1, and a semaphore with 0x8002000A.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD

static SceUID rwlock, release;
static volatile int lock_result[2], unlock_result[2];

static int in_rwlock_wait(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && (info.status & SCE_THREAD_WAITING) &&
           (info.waitType & ~WAIT_TYPE_CB) == WAIT_TYPE_RW_LOCK;
}

static SceKernelRWLockInfo rwlock_info(void) {
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    sceKernelGetRWLockInfo(rwlock, &info);
    return info;
}

// Waits for a read lock behind the write lock the main thread holds
static int read_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    lock_result[0] = sceKernelLockReadRWLock(rwlock, &timeout);
    return 0;
}

// Takes a read lock and keeps it until the test signals the release semaphore
static int holder(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    lock_result[i] = sceKernelLockReadRWLock(rwlock, &timeout);
    timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(release, 1, &timeout);
    unlock_result[i] = sceKernelUnlockReadRWLock(rwlock);
    return 0;
}

static void read_holds_case(void) {
    const char *name = "read holds";
    SceUID threads[2];
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    release = sceKernelCreateSema("release", 0, 0, 2, NULL);
    for (int i = 0; i < 2; i++) {
        lock_result[i] = unlock_result[i] = NOT_DONE;
        threads[i] = test_start_thread_arg(holder, i);
    }
    for (int i = 0; i < 2; i++)
        TEST_WAIT_FOR(test_in_wait(threads[i], WAIT_TYPE_SEMA, release));
    SceKernelRWLockInfo info = rwlock_info();
    PRINTF(info.lockCount, "%s: the lock count with two readers", name);

    SceUInt32 readers = NOT_DONE, writers = NOT_DONE;
    PRINTF(sceKernelCancelRWLock(rwlock, &readers, &writers, 0), "%s: cancel with flag 0", name);
    PRINTF(readers, "%s: the readers the cancel counted", name);
    PRINTF(writers, "%s: the writers the cancel counted", name);
    info = rwlock_info();
    PRINTF(info.lockCount, "%s: the lock count after the cancel", name);
    PRINTF(sceKernelTryLockWriteRWLock(rwlock), "%s: a try write lock after the cancel", name);
    PRINTF(sceKernelUnlockWriteRWLock(rwlock), "%s: the write unlock", name);

    sceKernelSignalSema(release, 2);
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < 2; i++) {
        PRINTF(lock_result[i], "%s: holder %d's read lock", name, i + 1);
        PRINTF(unlock_result[i], "%s: holder %d's read unlock after the cancel", name, i + 1);
    }
    info = rwlock_info();
    PRINTF(info.lockCount, "%s: the lock count at the end", name);
    sceKernelDeleteSema(release);
    sceKernelDeleteRWLock(rwlock);
}

static void report_flag1(const char *name) {
    const SceKernelRWLockInfo info = rwlock_info();
    PRINTF(info.lockCount, "%s: the lock count after", name);
    PRINTF(info.writeOwnerId == sceKernelGetThreadId(), "%s: the main thread owns the lock after", name);
}

static void flag1_cases(void) {
    // The lock is free
    const char *name = "flag 1 on a free lock";
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    SceUInt32 readers = NOT_DONE, writers = NOT_DONE;
    PRINTF(sceKernelCancelRWLock(rwlock, &readers, &writers, 1), "%s: cancel", name);
    PRINTF(readers, "%s: the readers the cancel counted", name);
    PRINTF(writers, "%s: the writers the cancel counted", name);
    report_flag1(name);
    PRINTF(sceKernelUnlockWriteRWLock(rwlock), "%s: the write unlock", name);
    PRINTF(rwlock_info().lockCount, "%s: the lock count after the unlock", name);
    sceKernelDeleteRWLock(rwlock);

    // Another thread holds a read lock
    name = "flag 1 while another thread reads";
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    release = sceKernelCreateSema("release", 0, 0, 1, NULL);
    lock_result[0] = unlock_result[0] = NOT_DONE;
    const SceUID reader = test_start_thread_arg(holder, 0);
    TEST_WAIT_FOR(test_in_wait(reader, WAIT_TYPE_SEMA, release));
    PRINTF(rwlock_info().lockCount, "%s: the lock count before", name);
    PRINTF(sceKernelCancelRWLock(rwlock, NULL, NULL, 1), "%s: cancel", name);
    report_flag1(name);
    sceKernelSignalSema(release, 1);
    test_join_thread(reader);
    PRINTF(unlock_result[0], "%s: the other thread's read unlock after", name);
    PRINTF(sceKernelUnlockWriteRWLock(rwlock), "%s: the write unlock", name);
    PRINTF(rwlock_info().lockCount, "%s: the lock count after the unlock", name);
    sceKernelDeleteSema(release);
    sceKernelDeleteRWLock(rwlock);

    // The caller holds a recursive write lock twice
    name = "flag 1 with a recursive write lock held twice";
    rwlock = sceKernelCreateRWLock("rwlock", RW_LOCK_ATTR_RECURSIVE, NULL);
    PRINTF(sceKernelLockWriteRWLock(rwlock, NULL), "%s: the first write lock", name);
    PRINTF(sceKernelLockWriteRWLock(rwlock, NULL), "%s: the second write lock", name);
    PRINTF(rwlock_info().lockCount, "%s: the lock count before", name);
    PRINTF(sceKernelCancelRWLock(rwlock, NULL, NULL, 1), "%s: cancel", name);
    report_flag1(name);
    PRINTF(sceKernelUnlockWriteRWLock(rwlock), "%s: the first write unlock", name);
    PRINTF(sceKernelUnlockWriteRWLock(rwlock), "%s: the second write unlock", name);
    PRINTF(rwlock_info().lockCount, "%s: the lock count after the unlocks", name);
    sceKernelDeleteRWLock(rwlock);
}

// A flag the call refuses must change nothing, so a reader that waits behind the main thread's write
// lock keeps waiting, and the lock is still the main thread's
static void bad_flags_case(void) {
    static const struct {
        SceInt32 flag;
        const char *name;
    } flags[] = { { 2, "flag 2" }, { -1, "flag -1" }, { 0x100, "flag 0x100" } };
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    sceKernelLockWriteRWLock(rwlock, NULL);
    lock_result[0] = NOT_DONE;
    const SceUID reader = test_start_thread(read_waiter);
    TEST_WAIT_FOR(in_rwlock_wait(reader));
    for (unsigned int i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        SceUInt32 readers = NOT_DONE, writers = NOT_DONE;
        PRINTF(sceKernelCancelRWLock(rwlock, &readers, &writers, flags[i].flag), "%s: cancel", flags[i].name);
        PRINTF(readers == NOT_DONE && writers == NOT_DONE, "%s: the count pointers are left alone", flags[i].name);
    }
    const SceKernelRWLockInfo info = rwlock_info();
    PRINT("bad flags: the lock count after", info.lockCount);
    PRINT("bad flags: the main thread owns the lock after", info.writeOwnerId == sceKernelGetThreadId());
    PRINT("bad flags: the reader still waits", in_rwlock_wait(reader));
    PRINT("bad flags: the write unlock", sceKernelUnlockWriteRWLock(rwlock));
    test_join_thread(reader);
    PRINT("bad flags: the reader's result after the unlock", lock_result[0]);
    sceKernelDeleteRWLock(rwlock);

    // Which error comes first, the flag or the UID
    PRINT("flag 2 on an unknown UID: cancel", sceKernelCancelRWLock(0x12345, NULL, NULL, 2));
    const SceUID sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    PRINT("flag 2 on a semaphore: cancel", sceKernelCancelRWLock(sema, NULL, NULL, 2));
    sceKernelDeleteSema(sema);
}

int main(void) {
    read_holds_case();
    flag1_cases();
    bad_flags_case();

    test_exit();
    return 0;
}
