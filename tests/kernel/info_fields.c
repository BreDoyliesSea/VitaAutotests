// The fields of each sync object's info: the attribute as given and as reported, the name for a 31,
// 32 and 40 character name, the initial and current values right after create and after a few
// calls, and the waiting thread counts with 0 and 2 waiters. It also prints how many bytes an info
// call writes with a size of 0 and with sizes smaller than its struct. Simple events and timers are
// in simple_event_info and timer_info_cancel.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NAME_31 "abcdefghijklmnopqrstuvwxyz01234"
#define NAME_32 NAME_31 "5"
#define NAME_40 NAME_32 "6789ABCD"

enum Kind { SEMA, EVENT_FLAG, MUTEX, LW_MUTEX, COND, LW_COND, RWLOCK, MSG_PIPE };
enum Wait { WAIT_SEMA, WAIT_FLAG, WAIT_MUTEX, WAIT_LW_MUTEX, WAIT_COND, WAIT_LW_COND, WAIT_READ, WAIT_WRITE, WAIT_RECEIVE, WAIT_SEND };

static SceUID sema, flag, mutex, cond, cond_mutex, rwlock, pipe;
static SceKernelLwMutexWork lw_mutex, lw_cond_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int results[3];

// Waits in the way given by the low byte of the argument, into the slot given by the next byte
static int waiter(SceSize args, void *argp) {
    (void)args;
    const int arg = *(const int *)argp;
    const int slot = arg >> 8;
    SceUInt timeout = TEST_WAIT_US;
    unsigned int bits;
    char buf[8];
    int result = 0;
    switch (arg & 0xFF) {
    case WAIT_SEMA: result = sceKernelWaitSema(sema, 1, &timeout); break;
    case WAIT_FLAG: result = sceKernelWaitEventFlag(flag, 0x100, SCE_EVENT_WAITAND, &bits, &timeout); break;
    case WAIT_MUTEX:
        result = sceKernelLockMutex(mutex, 1, &timeout);
        if (result == 0)
            sceKernelUnlockMutex(mutex, 1);
        break;
    case WAIT_LW_MUTEX:
        result = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        if (result == 0)
            sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case WAIT_COND:
        sceKernelLockMutex(cond_mutex, 1, NULL);
        result = sceKernelWaitCond(cond, &timeout);
        sceKernelUnlockMutex(cond_mutex, 1);
        break;
    case WAIT_LW_COND:
        sceKernelLockLwMutex(&lw_cond_mutex, 1, NULL);
        result = sceKernelWaitLwCond(&lw_cond, &timeout);
        sceKernelUnlockLwMutex(&lw_cond_mutex, 1);
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
    case WAIT_RECEIVE: result = sceKernelReceiveMsgPipe(pipe, buf, 4, MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    case WAIT_SEND: result = sceKernelSendMsgPipe(pipe, "12345678", 8, MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    }
    results[slot] = result;
    return 0;
}

// Starts a waiter of the given way into slot and returns once it waits
static SceUID start_waiter(int wait, int slot) {
    results[slot] = -1;
    const SceUID thread = test_start_thread_arg(waiter, wait | (slot << 8));
    test_wait_until_waiting(thread);
    return thread;
}

// 0 for no thread, 1 for the calling thread, 2 for another one
static int owner(SceUID id) {
    return id == 0 ? 0 : id == sceKernelGetThreadId() ? 1 : 2;
}

static int name_length(const char *name) {
    int len = 0;
    while (len < 32 && name[len] != '\0')
        len++;
    return len;
}

// Creates an object of the kind named name, and returns the length of the name its info reports,
// or the create error. *matches gets whether the reported name is the start of name.
static int reported_name(int kind, const char *name, int *matches) {
    char reported[32];
    SceUID id;
    int result;
    static SceKernelLwMutexWork lw_work;
    static SceKernelLwCondWork lw_cond_work;
    switch (kind) {
    case SEMA: {
        if ((id = sceKernelCreateSema(name, 0, 0, 1, NULL)) < 0)
            return id;
        SceKernelSemaInfo info = { .size = sizeof(info) };
        sceKernelGetSemaInfo(id, &info);
        sceClibMemcpy(reported, info.name, 32);
        sceKernelDeleteSema(id);
        break;
    }
    case EVENT_FLAG: {
        if ((id = sceKernelCreateEventFlag(name, 0, 0, NULL)) < 0)
            return id;
        SceKernelEventFlagInfo info = { .size = sizeof(info) };
        sceKernelGetEventFlagInfo(id, &info);
        sceClibMemcpy(reported, info.name, 32);
        sceKernelDeleteEventFlag(id);
        break;
    }
    case MUTEX: {
        if ((id = sceKernelCreateMutex(name, 0, 0, NULL)) < 0)
            return id;
        MutexInfo info = { .size = sizeof(info) };
        sceKernelGetMutexInfo(id, (SceKernelMutexInfo *)&info);
        sceClibMemcpy(reported, info.name, 32);
        sceKernelDeleteMutex(id);
        break;
    }
    case LW_MUTEX: {
        if ((result = sceKernelCreateLwMutex(&lw_work, name, 0, 0, NULL)) < 0)
            return result;
        LwMutexInfo info = { .size = sizeof(info) };
        sceKernelGetLwMutexInfo(&lw_work, &info);
        sceClibMemcpy(reported, info.name, 32);
        sceKernelDeleteLwMutex(&lw_work);
        break;
    }
    case COND: {
        if ((id = sceKernelCreateCond(name, 0, cond_mutex, NULL)) < 0)
            return id;
        SceKernelCondInfo info = { .size = sizeof(info) };
        sceKernelGetCondInfo(id, &info);
        sceClibMemcpy(reported, info.name, 32);
        sceKernelDeleteCond(id);
        break;
    }
    case LW_COND: {
        if ((result = sceKernelCreateLwCond(&lw_cond_work, name, 0, &lw_cond_mutex, NULL)) < 0)
            return result;
        LwCondInfo info = { .size = sizeof(info) };
        sceKernelGetLwCondInfo(&lw_cond_work, &info);
        sceClibMemcpy(reported, info.name, 32);
        sceKernelDeleteLwCond(&lw_cond_work);
        break;
    }
    case RWLOCK: {
        if ((id = sceKernelCreateRWLock(name, 0, NULL)) < 0)
            return id;
        SceKernelRWLockInfo info = { .size = sizeof(info) };
        sceKernelGetRWLockInfo(id, &info);
        sceClibMemcpy(reported, info.name, 32);
        sceKernelDeleteRWLock(id);
        break;
    }
    case MSG_PIPE: {
        if ((id = sceKernelCreateMsgPipe(name, MSG_PIPE_TYPE_USER, 0, 16, NULL)) < 0)
            return id;
        SceKernelMppInfo info = { .size = sizeof(info) };
        sceKernelGetMsgPipeInfo(id, &info);
        sceClibMemcpy(reported, info.name, 32);
        sceKernelDeleteMsgPipe(id);
        break;
    }
    default: return 0;
    }
    const int len = name_length(reported);
    *matches = sceClibMemcmp(reported, name, len) == 0;
    return len;
}

static void print_names(int kind, const char *what) {
    static const char *const names[] = { NAME_31, NAME_32, NAME_40 };
    static const int lengths[] = { 31, 32, 40 };
    for (int i = 0; i < 3; i++) {
        int matches = 0;
        const int len = reported_name(kind, names[i], &matches);
        PRINTF(len, "%s: the reported length of a %d character name", what, lengths[i]);
        if (len >= 0)
            PRINTF(matches, "%s: it is the start of the %d character name", what, lengths[i]);
    }
}

// The info call of the object a size check runs on
typedef int (*InfoCall)(void *info);

static int sema_info(void *info) {
    return sceKernelGetSemaInfo(sema, info);
}
static int flag_info(void *info) {
    return sceKernelGetEventFlagInfo(flag, info);
}
static int mutex_info(void *info) {
    return sceKernelGetMutexInfo(mutex, info);
}
static int lw_mutex_info(void *info) {
    return sceKernelGetLwMutexInfo(&lw_mutex, info);
}
static int cond_info(void *info) {
    return sceKernelGetCondInfo(cond, info);
}
static int lw_cond_info(void *info) {
    return sceKernelGetLwCondInfo(&lw_cond, info);
}
static int rwlock_info(void *info) {
    return sceKernelGetRWLockInfo(rwlock, info);
}
static int pipe_info(void *info) {
    return sceKernelGetMsgPipeInfo(pipe, info);
}

// Runs an info call with the size field set to each size below the struct's, twice, each time into
// a buffer filled with another byte, and prints its result and how far past the size field it wrote
static void print_sizes(const char *what, InfoCall call, SceSize struct_size) {
    static union {
        SceUInt64 align;
        unsigned char bytes[128];
    } buf[2];
    const SceSize sizes[] = { 0, 4, 8, 40, struct_size - 4 };
    for (unsigned int s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int result = 0;
        for (int pass = 0; pass < 2; pass++) {
            sceClibMemset(buf[pass].bytes, pass ? 0x11 : 0xEE, sizeof(buf[pass].bytes));
            *(SceSize *)buf[pass].bytes = sizes[s];
            result = call(buf[pass].bytes);
        }
        int written = 0;
        for (int i = sizeof(SceSize); i < (int)sizeof(buf[0].bytes); i++) {
            if (buf[0].bytes[i] != 0xEE || buf[1].bytes[i] != 0x11)
                written = i + 1;
        }
        PRINTF(result, "%s: info with a size of %d", what, (int)sizes[s]);
        PRINTF(written, "%s: the end of what it writes with a size of %d", what, (int)sizes[s]);
    }
}

static void join(SceUID *threads, int count) {
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
}

static void test_sema(void) {
    SceUID threads[2];
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sema = sceKernelCreateSema("sema", SCE_KERNEL_ATTR_THREAD_PRIO, 2, 7, NULL);
    PRINT("semaphore: info", sceKernelGetSemaInfo(sema, &info));
    PRINT("semaphore: the attribute", info.attr);
    PRINT("semaphore: the initial count", info.initCount);
    PRINT("semaphore: the count", info.currentCount);
    PRINT("semaphore: the maximum", info.maxCount);
    PRINT("semaphore: the waiting threads", info.numWaitThreads);
    sceKernelPollSema(sema, 1);
    sceKernelSignalSema(sema, 3);
    sceKernelGetSemaInfo(sema, &info);
    PRINT("semaphore: the count after taking 1 and giving 3", info.currentCount);
    PRINT("semaphore: the initial count after it", info.initCount);
    sceKernelPollSema(sema, 4);
    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_SEMA, i);
    sceKernelGetSemaInfo(sema, &info);
    PRINT("semaphore: the count with 2 waiters", info.currentCount);
    PRINT("semaphore: the waiting threads with 2 waiters", info.numWaitThreads);
    sceKernelSignalSema(sema, 2);
    join(threads, 2);
    sceKernelGetSemaInfo(sema, &info);
    PRINT("semaphore: the waiting threads after they got it", info.numWaitThreads);
    print_names(SEMA, "semaphore");
    print_sizes("semaphore", sema_info, sizeof(SceKernelSemaInfo));
    sceKernelDeleteSema(sema);
}

static void test_flag(void) {
    SceUID threads[2];
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    flag = sceKernelCreateEventFlag("flag", SCE_EVENT_WAITMULTIPLE | SCE_KERNEL_ATTR_THREAD_PRIO, 0x5, NULL);
    PRINT("event flag: info", sceKernelGetEventFlagInfo(flag, &info));
    PRINT("event flag: the attribute", info.attr);
    PRINT("event flag: the initial pattern", info.initPattern);
    PRINT("event flag: the pattern", info.currentPattern);
    PRINT("event flag: the waiting threads", info.numWaitThreads);
    sceKernelSetEventFlag(flag, 0x10);
    sceKernelClearEventFlag(flag, 0x14);
    sceKernelGetEventFlagInfo(flag, &info);
    PRINT("event flag: the pattern after a set and a clear", info.currentPattern);
    PRINT("event flag: the initial pattern after them", info.initPattern);
    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_FLAG, i);
    sceKernelGetEventFlagInfo(flag, &info);
    PRINT("event flag: the waiting threads with 2 waiters", info.numWaitThreads);
    sceKernelSetEventFlag(flag, 0x100);
    join(threads, 2);
    sceKernelGetEventFlagInfo(flag, &info);
    PRINT("event flag: the waiting threads after they woke", info.numWaitThreads);
    PRINT("event flag: the pattern after they woke", info.currentPattern);
    print_names(EVENT_FLAG, "event flag");
    print_sizes("event flag", flag_info, sizeof(SceKernelEventFlagInfo));
    sceKernelDeleteEventFlag(flag);
}

static void test_mutex(void) {
    SceUID threads[2];
    MutexInfo info = { .size = sizeof(info) };
    mutex = sceKernelCreateMutex("mutex", SCE_KERNEL_MUTEX_ATTR_RECURSIVE | SCE_KERNEL_ATTR_THREAD_PRIO, 2, NULL);
    PRINT("mutex: info", sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info));
    PRINT("mutex: the attribute", info.attr);
    PRINT("mutex: the initial count", info.initCount);
    PRINT("mutex: the count", info.currentCount);
    PRINT("mutex: the owner", owner(info.currentOwnerId));
    PRINT("mutex: the waiting threads", info.numWaitThreads);
    PRINT("mutex: the ceiling priority", info.ceilingPriority);
    sceKernelUnlockMutex(mutex, 2);
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    PRINT("mutex: the count after unlocking it", info.currentCount);
    PRINT("mutex: the owner after unlocking it", owner(info.currentOwnerId));
    sceKernelLockMutex(mutex, 3, NULL);
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    PRINT("mutex: the count after locking it 3 times", info.currentCount);
    PRINT("mutex: the initial count after it", info.initCount);
    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_MUTEX, i);
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    PRINT("mutex: the waiting threads with 2 waiters", info.numWaitThreads);
    sceKernelUnlockMutex(mutex, 3);
    join(threads, 2);
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    PRINT("mutex: the waiting threads after they got it", info.numWaitThreads);
    PRINT("mutex: the owner after they unlocked it", owner(info.currentOwnerId));
    print_names(MUTEX, "mutex");
    print_sizes("mutex", mutex_info, sizeof(MutexInfo));
    sceKernelDeleteMutex(mutex);
}

static void test_lw_mutex(void) {
    SceUID threads[2];
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelCreateLwMutex(&lw_mutex, "lw mutex", LW_MUTEX_ATTR_RECURSIVE, 1, NULL);
    PRINT("lightweight mutex: info", sceKernelGetLwMutexInfo(&lw_mutex, &info));
    PRINT("lightweight mutex: the attribute", info.attr);
    PRINT("lightweight mutex: the work area", info.work == &lw_mutex);
    PRINT("lightweight mutex: the initial count", info.initCount);
    PRINT("lightweight mutex: the count", info.currentCount);
    PRINT("lightweight mutex: the owner", owner(info.currentOwnerId));
    PRINT("lightweight mutex: the waiting threads", info.numWaitThreads);
    LwMutexInfo by_id = { .size = sizeof(by_id) };
    PRINT("lightweight mutex: info by UID", sceKernelGetLwMutexInfoById(info.uid, &by_id));
    PRINT("lightweight mutex: info by UID is the same", sceClibMemcmp(&info, &by_id, sizeof(info)) == 0);
    sceKernelLockLwMutex(&lw_mutex, 2, NULL);
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    PRINT("lightweight mutex: the count after locking it twice more", info.currentCount);
    sceKernelUnlockLwMutex(&lw_mutex, 3);
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    PRINT("lightweight mutex: the count after unlocking it", info.currentCount);
    PRINT("lightweight mutex: the owner after unlocking it", owner(info.currentOwnerId));
    PRINT("lightweight mutex: the initial count after it", info.initCount);
    sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_LW_MUTEX, i);
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    PRINT("lightweight mutex: the waiting threads with 2 waiters", info.numWaitThreads);
    sceKernelUnlockLwMutex(&lw_mutex, 1);
    join(threads, 2);
    PRINT("lightweight mutex: the first waiter got it", results[0]);
    PRINT("lightweight mutex: the second waiter got it", results[1]);
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    PRINT("lightweight mutex: the waiting threads after they got it", info.numWaitThreads);
    print_names(LW_MUTEX, "lightweight mutex");
    print_sizes("lightweight mutex", lw_mutex_info, sizeof(LwMutexInfo));
    sceKernelDeleteLwMutex(&lw_mutex);
}

static void test_cond(void) {
    SceUID threads[2];
    SceKernelCondInfo info = { .size = sizeof(info) };
    cond = sceKernelCreateCond("cond", SCE_KERNEL_ATTR_THREAD_PRIO, cond_mutex, NULL);
    PRINT("condition variable: info", sceKernelGetCondInfo(cond, &info));
    PRINT("condition variable: the attribute", info.attr);
    PRINT("condition variable: the mutex", info.mutexId == cond_mutex);
    PRINT("condition variable: the waiting threads", info.numWaitThreads);
    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_COND, i);
    sceKernelGetCondInfo(cond, &info);
    PRINT("condition variable: the waiting threads with 2 waiters", info.numWaitThreads);
    sceKernelSignalCondAll(cond);
    join(threads, 2);
    sceKernelGetCondInfo(cond, &info);
    PRINT("condition variable: the waiting threads after they woke", info.numWaitThreads);
    print_names(COND, "condition variable");
    print_sizes("condition variable", cond_info, sizeof(SceKernelCondInfo));
    sceKernelDeleteCond(cond);
}

static void test_lw_cond(void) {
    SceUID threads[2];
    LwCondInfo info = { .size = sizeof(info) };
    sceKernelCreateLwCond(&lw_cond, "lw cond", 0, &lw_cond_mutex, NULL);
    PRINT("lightweight condition variable: info", sceKernelGetLwCondInfo(&lw_cond, &info));
    PRINT("lightweight condition variable: the attribute", info.attr);
    PRINT("lightweight condition variable: the work area", info.work == &lw_cond);
    PRINT("lightweight condition variable: the mutex", info.lwMutex == &lw_cond_mutex);
    PRINT("lightweight condition variable: the waiting threads", info.numWaitThreads);
    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_LW_COND, i);
    sceKernelGetLwCondInfo(&lw_cond, &info);
    PRINT("lightweight condition variable: the waiting threads with 2 waiters", info.numWaitThreads);
    sceKernelSignalLwCondAll(&lw_cond);
    join(threads, 2);
    sceKernelGetLwCondInfo(&lw_cond, &info);
    PRINT("lightweight condition variable: the waiting threads after they woke", info.numWaitThreads);
    print_names(LW_COND, "lightweight condition variable");
    print_sizes("lightweight condition variable", lw_cond_info, sizeof(LwCondInfo));
    sceKernelDeleteLwCond(&lw_cond);
}

static void test_rwlock(void) {
    SceUID threads[3];
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    rwlock = sceKernelCreateRWLock("rwlock", SCE_KERNEL_ATTR_THREAD_PRIO, NULL);
    PRINT("reader/writer lock: info", sceKernelGetRWLockInfo(rwlock, &info));
    PRINT("reader/writer lock: the attribute", info.attr);
    PRINT("reader/writer lock: the lock count", info.lockCount);
    PRINT("reader/writer lock: the writer", owner(info.writeOwnerId));
    PRINT("reader/writer lock: the waiting readers", info.numReadWaitThreads);
    PRINT("reader/writer lock: the waiting writers", info.numWriteWaitThreads);
    sceKernelLockReadRWLock(rwlock, NULL);
    sceKernelLockReadRWLock(rwlock, NULL);
    sceKernelGetRWLockInfo(rwlock, &info);
    PRINT("reader/writer lock: the lock count with 2 read locks", info.lockCount);
    PRINT("reader/writer lock: the writer with 2 read locks", owner(info.writeOwnerId));
    sceKernelUnlockReadRWLock(rwlock);
    sceKernelUnlockReadRWLock(rwlock);
    sceKernelLockWriteRWLock(rwlock, NULL);
    sceKernelGetRWLockInfo(rwlock, &info);
    PRINT("reader/writer lock: the lock count with the write lock", info.lockCount);
    PRINT("reader/writer lock: the writer with the write lock", owner(info.writeOwnerId));
    threads[0] = start_waiter(WAIT_READ, 0);
    threads[1] = start_waiter(WAIT_READ, 1);
    threads[2] = start_waiter(WAIT_WRITE, 2);
    sceKernelGetRWLockInfo(rwlock, &info);
    PRINT("reader/writer lock: the waiting readers with 2 of them", info.numReadWaitThreads);
    PRINT("reader/writer lock: the waiting writers with 1 of them", info.numWriteWaitThreads);
    sceKernelUnlockWriteRWLock(rwlock);
    join(threads, 3);
    sceKernelGetRWLockInfo(rwlock, &info);
    PRINT("reader/writer lock: the lock count after they unlocked", info.lockCount);
    PRINT("reader/writer lock: the waiting readers after", info.numReadWaitThreads);
    PRINT("reader/writer lock: the waiting writers after", info.numWriteWaitThreads);
    print_names(RWLOCK, "reader/writer lock");
    print_sizes("reader/writer lock", rwlock_info, sizeof(SceKernelRWLockInfo));
    sceKernelDeleteRWLock(rwlock);
}

static void test_pipe(void) {
    static char buf[4096];
    SceUID threads[2];
    SceSize size;
    SceKernelMppInfo info = { .size = sizeof(info) };
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0x8, 100, NULL);
    PRINT("message pipe: info", sceKernelGetMsgPipeInfo(pipe, &info));
    PRINT("message pipe: the attribute", info.attr);
    PRINT("message pipe: the buffer size", info.bufSize);
    PRINT("message pipe: the free size", info.freeSize);
    PRINT("message pipe: the waiting senders", info.numSendWaitThreads);
    PRINT("message pipe: the waiting receivers", info.numReceiveWaitThreads);
    sceKernelTrySendMsgPipe(pipe, buf, 10, MSG_PIPE_MODE_FULL, &size);
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINT("message pipe: the free size after sending 10", info.freeSize);
    sceKernelTryReceiveMsgPipe(pipe, buf, 4, MSG_PIPE_MODE_FULL, &size);
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINT("message pipe: the free size after receiving 4", info.freeSize);
    sceKernelTryReceiveMsgPipe(pipe, buf, 6, MSG_PIPE_MODE_FULL, &size);
    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_RECEIVE, i);
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINT("message pipe: the waiting receivers with 2 of them", info.numReceiveWaitThreads);
    PRINT("message pipe: the waiting senders with them", info.numSendWaitThreads);
    sceKernelSendMsgPipe(pipe, "abcdefgh", 8, MSG_PIPE_MODE_FULL, NULL, NULL);
    join(threads, 2);
    PRINT("message pipe: the first receiver got its part", results[0]);
    PRINT("message pipe: the second receiver got its part", results[1]);
    // Fill the pipe so senders wait
    sceKernelGetMsgPipeInfo(pipe, &info);
    const int capacity = info.freeSize;
    sceKernelTrySendMsgPipe(pipe, buf, capacity, MSG_PIPE_MODE_FULL, &size);
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINT("message pipe: the free size when full", info.freeSize);
    for (int i = 0; i < 2; i++)
        threads[i] = start_waiter(WAIT_SEND, i);
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINT("message pipe: the waiting senders with 2 of them", info.numSendWaitThreads);
    PRINT("message pipe: the waiting receivers with them", info.numReceiveWaitThreads);
    sceKernelTryReceiveMsgPipe(pipe, buf, capacity, MSG_PIPE_MODE_FULL, &size);
    join(threads, 2);
    PRINT("message pipe: the first sender sent", results[0]);
    PRINT("message pipe: the second sender sent", results[1]);
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINT("message pipe: the free size after both sent", info.freeSize);
    print_names(MSG_PIPE, "message pipe");
    print_sizes("message pipe", pipe_info, sizeof(SceKernelMppInfo));
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    cond_mutex = sceKernelCreateMutex("cond mutex", 0, 0, NULL);
    sceKernelCreateLwMutex(&lw_cond_mutex, "lw cond mutex", 0, 0, NULL);

    test_sema();
    test_flag();
    test_mutex();
    test_lw_mutex();
    test_cond();
    test_lw_cond();
    test_rwlock();
    test_pipe();

    sceKernelDeleteLwMutex(&lw_cond_mutex);
    sceKernelDeleteMutex(cond_mutex);
    test_exit();
    return 0;
}
