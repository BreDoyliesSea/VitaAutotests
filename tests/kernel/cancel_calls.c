// The cancel call of each semaphore, event flag, mutex, reader/writer lock and message pipe, with 2
// waiters, with one waiter and no count pointer, and with values it may reject: what the waiters get
// back, the waiting thread counts written back, and the count, pattern, owner or data left after it.
// Condition variables have no cancel call. Simple events and timers are in simple_event_cancel and
// timer_info_cancel.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Wait { WAIT_SEMA, WAIT_FLAG_AND, WAIT_FLAG_OR_CLEAR, WAIT_MUTEX, WAIT_READ, WAIT_WRITE, WAIT_RECEIVE };

static SceUID sema, flag, mutex, rwlock, pipe, release;
static volatile int results[3];
static unsigned int extra[3];
static volatile int holder_result;

// Waits in the way given by the low byte of the argument, into the slot given by the next byte.
// extra gets the bits of an event flag wait or the size of a receive.
static int waiter(SceSize args, void *argp) {
    (void)args;
    const int arg = *(const int *)argp;
    const int slot = arg >> 8;
    SceUInt timeout = TEST_WAIT_US;
    char buf[8];
    int result = 0;
    switch (arg & 0xFF) {
    case WAIT_SEMA: result = sceKernelWaitSema(sema, 1, &timeout); break;
    case WAIT_FLAG_AND: result = sceKernelWaitEventFlag(flag, 0x2, SCE_EVENT_WAITAND, &extra[slot], &timeout); break;
    case WAIT_FLAG_OR_CLEAR: result = sceKernelWaitEventFlag(flag, 0x4, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &extra[slot], &timeout); break;
    case WAIT_MUTEX:
        result = sceKernelLockMutex(mutex, 1, &timeout);
        if (result == 0)
            sceKernelUnlockMutex(mutex, 1);
        break;
    case WAIT_READ:
        result = sceKernelLockReadRWLock(rwlock, &timeout);
        if (result == 0)
            sceKernelUnlockReadRWLock(rwlock);
        break;
    case WAIT_WRITE:
        result = sceKernelLockWriteRWLock(rwlock, &timeout);
        if (result == 0)
            sceKernelUnlockWriteRWLock(rwlock);
        break;
    case WAIT_RECEIVE: result = sceKernelReceiveMsgPipe(pipe, buf, 8, MSG_PIPE_MODE_FULL, &extra[slot], &timeout); break;
    }
    results[slot] = result;
    return 0;
}

static SceUID start_waiter(int wait, int slot) {
    results[slot] = -1;
    extra[slot] = 0xDEAD;
    const SceUID thread = test_start_thread_arg(waiter, wait | (slot << 8));
    test_wait_until_waiting(thread);
    return thread;
}

static void join(SceUID *threads, int count) {
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
}

// Holds the mutex or a read lock of the reader/writer lock until release is signaled, then stores
// the result of giving it back
static int mutex_holder(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelLockMutex(mutex, 1, NULL);
    sceKernelWaitSema(release, 1, &timeout);
    holder_result = sceKernelUnlockMutex(mutex, 1);
    return 0;
}

static int read_holder(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelLockReadRWLock(rwlock, NULL);
    sceKernelWaitSema(release, 1, &timeout);
    holder_result = sceKernelUnlockReadRWLock(rwlock);
    return 0;
}

// 0 for no thread, 1 for the calling thread, 2 for another one
static int owner(SceUID id) {
    return id == 0 ? 0 : id == sceKernelGetThreadId() ? 1 : 2;
}

static SceKernelSemaInfo sema_info(void) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(sema, &info);
    return info;
}

static MutexInfo mutex_info(void) {
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    return info;
}

static SceKernelRWLockInfo rwlock_info(void) {
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    sceKernelGetRWLockInfo(rwlock, &info);
    return info;
}

static unsigned int flag_pattern(void) {
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(flag, &info);
    return info.currentPattern;
}

static void test_sema(void) {
    SceUID threads[2];
    int waiting;
    sema = sceKernelCreateSema("sema", 0, 0, 3, NULL);

    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_SEMA, i);
    waiting = 0xDEAD;
    PRINT("semaphore: cancel to 2 with 2 waiters", sceKernelCancelSema(sema, 2, &waiting));
    join(threads, 2);
    PRINT("semaphore: the waiting count", waiting);
    PRINT("semaphore: the first waiter", results[0]);
    PRINT("semaphore: the second waiter", results[1]);
    PRINT("semaphore: the count after", sema_info().currentCount);
    PRINT("semaphore: the initial count after", sema_info().initCount);

    sceKernelCancelSema(sema, 0, NULL);
    threads[0] = start_waiter(WAIT_SEMA, 0);
    PRINT("semaphore: cancel to 3 without a count pointer with a waiter", sceKernelCancelSema(sema, 3, NULL));
    join(threads, 1);
    PRINT("semaphore: the waiter", results[0]);
    PRINT("semaphore: the count after", sema_info().currentCount);

    sceKernelCancelSema(sema, 0, NULL);
    threads[0] = start_waiter(WAIT_SEMA, 0);
    waiting = 0xDEAD;
    PRINT("semaphore: cancel past the maximum with a waiter", sceKernelCancelSema(sema, 4, &waiting));
    PRINT("semaphore: the waiting count", waiting);
    PRINT("semaphore: the waiter still waits", test_in_wait(threads[0], WAIT_TYPE_SEMA, sema));
    PRINT("semaphore: the count after", sema_info().currentCount);
    PRINT("semaphore: cancel to -2 with a waiter", sceKernelCancelSema(sema, -2, &waiting));
    join(threads, 1);
    PRINT("semaphore: the waiter", results[0]);
    PRINT("semaphore: the count after", sema_info().currentCount);

    sceKernelDeleteSema(sema);
}

static void test_flag(void) {
    SceUID threads[2];
    SceUInt32 waiting;
    flag = sceKernelCreateEventFlag("flag", SCE_EVENT_WAITMULTIPLE, 0x1, NULL);

    threads[0] = start_waiter(WAIT_FLAG_AND, 0);
    threads[1] = start_waiter(WAIT_FLAG_OR_CLEAR, 1);
    waiting = 0xDEAD;
    PRINT("event flag: cancel to 0x36 with 2 waiters", sceKernelCancelEventFlag(flag, 0x36, &waiting));
    join(threads, 2);
    PRINT("event flag: the waiting count", waiting);
    PRINT("event flag: the AND waiter", results[0]);
    PRINT("event flag: the bits it got", extra[0]);
    PRINT("event flag: the OR waiter that clears", results[1]);
    PRINT("event flag: the bits it got", extra[1]);
    PRINT("event flag: the pattern after", flag_pattern());
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(flag, &info);
    PRINT("event flag: the initial pattern after", info.initPattern);

    sceKernelClearEventFlag(flag, 0);
    threads[0] = start_waiter(WAIT_FLAG_AND, 0);
    PRINT("event flag: cancel to 0 without a count pointer with a waiter", sceKernelCancelEventFlag(flag, 0, NULL));
    join(threads, 1);
    PRINT("event flag: the waiter", results[0]);
    PRINT("event flag: the bits it got", extra[0]);
    PRINT("event flag: the pattern after", flag_pattern());

    sceKernelDeleteEventFlag(flag);
}

static void test_mutex(void) {
    SceUID threads[2];
    int waiting;
    MutexInfo info;
    mutex = sceKernelCreateMutex("mutex", 0, 1, NULL);

    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_MUTEX, i);
    waiting = 0xDEAD;
    PRINT("mutex: cancel to 0 with 2 waiters", sceKernelCancelMutex(mutex, 0, &waiting));
    join(threads, 2);
    PRINT("mutex: the waiting count", waiting);
    PRINT("mutex: the first waiter", results[0]);
    PRINT("mutex: the second waiter", results[1]);
    info = mutex_info();
    PRINT("mutex: the count after", info.currentCount);
    PRINT("mutex: the owner after", owner(info.currentOwnerId));

    // Another thread holds it
    holder_result = -1;
    const SceUID holder = test_start_thread(mutex_holder);
    test_wait_until_waiting(holder);
    PRINT("mutex: cancel to 1 while another thread holds it", sceKernelCancelMutex(mutex, 1, NULL));
    info = mutex_info();
    PRINT("mutex: the count after", info.currentCount);
    PRINT("mutex: the owner after", owner(info.currentOwnerId));
    sceKernelSignalSema(release, 1);
    test_join_thread(holder);
    PRINT("mutex: the other thread unlocks it after", holder_result);
    info = mutex_info();
    PRINT("mutex: the owner after that", owner(info.currentOwnerId));
    PRINT("mutex: unlock it", sceKernelUnlockMutex(mutex, 1));

    // Back to the initial count, which was 1
    PRINT("mutex: cancel to -1", sceKernelCancelMutex(mutex, -1, NULL));
    info = mutex_info();
    PRINT("mutex: the count after", info.currentCount);
    PRINT("mutex: the owner after", owner(info.currentOwnerId));
    sceKernelCancelMutex(mutex, 1, NULL);

    threads[0] = start_waiter(WAIT_MUTEX, 0);
    PRINT("mutex: cancel to 1 without a count pointer with a waiter", sceKernelCancelMutex(mutex, 1, NULL));
    join(threads, 1);
    PRINT("mutex: the waiter", results[0]);
    info = mutex_info();
    PRINT("mutex: the owner after", owner(info.currentOwnerId));

    sceKernelDeleteMutex(mutex);

    // A recursive mutex canceled to a count above 1
    mutex = sceKernelCreateMutex("recursive", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    PRINT("recursive mutex: cancel to 0x7FFFFFFF", sceKernelCancelMutex(mutex, 0x7FFFFFFF, NULL));
    info = mutex_info();
    PRINT("recursive mutex: the count after", info.currentCount);
    PRINT("recursive mutex: the owner after", owner(info.currentOwnerId));
    PRINT("recursive mutex: lock it once more", sceKernelTryLockMutex(mutex, 1));
    sceKernelDeleteMutex(mutex);
}

static void test_rwlock(void) {
    SceUID threads[3];
    SceUInt32 readers, writers;
    SceKernelRWLockInfo info;
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);

    sceKernelLockWriteRWLock(rwlock, NULL);
    threads[0] = start_waiter(WAIT_READ, 0);
    threads[1] = start_waiter(WAIT_READ, 1);
    threads[2] = start_waiter(WAIT_WRITE, 2);
    readers = writers = 0xDEAD;
    PRINT("reader/writer lock: cancel with 2 waiting readers and a writer", sceKernelCancelRWLock(rwlock, &readers, &writers, 0));
    join(threads, 3);
    PRINT("reader/writer lock: the waiting readers", readers);
    PRINT("reader/writer lock: the waiting writers", writers);
    PRINT("reader/writer lock: the first reader", results[0]);
    PRINT("reader/writer lock: the second reader", results[1]);
    PRINT("reader/writer lock: the writer", results[2]);
    info = rwlock_info();
    PRINT("reader/writer lock: the lock count after", info.lockCount);
    PRINT("reader/writer lock: the writer after", owner(info.writeOwnerId));

    // Another thread holds a read lock, and the cancel gives the write lock to the caller
    holder_result = -1;
    const SceUID holder = test_start_thread(read_holder);
    test_wait_until_waiting(holder);
    PRINT("reader/writer lock: cancel with flag 1 while another thread reads", sceKernelCancelRWLock(rwlock, NULL, NULL, 1));
    info = rwlock_info();
    PRINT("reader/writer lock: the lock count after", info.lockCount);
    PRINT("reader/writer lock: the writer after", owner(info.writeOwnerId));
    sceKernelSignalSema(release, 1);
    test_join_thread(holder);
    PRINT("reader/writer lock: the other thread's read unlock after", holder_result);
    PRINT("reader/writer lock: write unlock", sceKernelUnlockWriteRWLock(rwlock));

    PRINT("reader/writer lock: cancel with flag -1", sceKernelCancelRWLock(rwlock, NULL, NULL, -1));
    PRINT("reader/writer lock: cancel with flag 0x100", sceKernelCancelRWLock(rwlock, NULL, NULL, 0x100));
    info = rwlock_info();
    PRINT("reader/writer lock: the lock count after", info.lockCount);
    PRINT("reader/writer lock: the writer after", owner(info.writeOwnerId));
    sceKernelCancelRWLock(rwlock, NULL, NULL, 0);

    sceKernelLockReadRWLock(rwlock, NULL);
    threads[0] = start_waiter(WAIT_WRITE, 0);
    writers = 0xDEAD;
    PRINT("reader/writer lock: cancel with only a writer count pointer", sceKernelCancelRWLock(rwlock, NULL, &writers, 0));
    join(threads, 1);
    PRINT("reader/writer lock: the waiting writers", writers);
    PRINT("reader/writer lock: the writer", results[0]);
    PRINT("reader/writer lock: read unlock after", sceKernelUnlockReadRWLock(rwlock));

    sceKernelDeleteRWLock(rwlock);
}

static void test_pipe(void) {
    SceUID threads[2];
    int senders, receivers;
    char buf[8];
    SceSize size;
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL);

    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_RECEIVE, i);
    senders = receivers = 0xDEAD;
    PRINT("message pipe: cancel with 2 receivers", sceKernelCancelMsgPipe(pipe, &senders, &receivers));
    join(threads, 2);
    PRINT("message pipe: the waiting senders", senders);
    PRINT("message pipe: the waiting receivers", receivers);
    PRINT("message pipe: the first receiver", results[0]);
    PRINT("message pipe: the size it got", extra[0]);
    PRINT("message pipe: the second receiver", results[1]);
    PRINT("message pipe: the size it got", extra[1]);

    // Part of what the receiver waits for is there
    sceKernelTrySendMsgPipe(pipe, "abc", 3, MSG_PIPE_MODE_FULL, &size);
    threads[0] = start_waiter(WAIT_RECEIVE, 0);
    receivers = 0xDEAD;
    PRINT("message pipe: cancel with only a receiver count pointer", sceKernelCancelMsgPipe(pipe, NULL, &receivers));
    join(threads, 1);
    PRINT("message pipe: the waiting receivers", receivers);
    PRINT("message pipe: the receiver that had part of its data", results[0]);
    PRINT("message pipe: the size it got", extra[0]);
    PRINT("message pipe: try receive the data after", sceKernelTryReceiveMsgPipe(pipe, buf, 1, MSG_PIPE_MODE_ASAP, &size));

    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    release = sceKernelCreateSema("release", 0, 0, 1, NULL);

    test_sema();
    test_flag();
    test_mutex();
    test_rwlock();
    test_pipe();

    sceKernelDeleteSema(release);
    test_exit();
    return 0;
}
