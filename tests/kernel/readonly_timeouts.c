// Waits given a timeout in memory the app can only read. The kernel reads a timeout and writes the
// time left back, which it can't do there. The memory is a block opened with sceKernelOpenMemBlock
// with flags 0x10, which maps the memory of a 64 KiB USER_RW block with ENABLE_OPEN read only, so
// the test sets the timeout through the original and sees through it whether a call wrote it.
// Each case, on fresh objects, prints the result, whether the call changed the timeout or the
// words after it, and what the wait did anyway: the count, pattern, lock or message it took. The
// cases are waits that can end at once, with a timeout of 1 s: a semaphore, a mutex, an event flag
// with clear, a message pipe receive and send, a read lock, a thread end, an auto reset simple
// event and several events. Then waits that block, each on a thread of its own: a semaphore wait
// that times out after 10 ms, a semaphore wait with a timeout of 5 s woken by a signal, and a
// condition wait that times out after 10 ms, which also prints whether the waiter holds the mutex
// again after. The test waits 1 s for such a thread to end, and if it hasn't, it deletes the
// object so the thread ends either way, and prints whether that was needed. Last, a lightweight
// mutex that can be locked at once, in case its wait code touches the timeout before the kernel
// does. The app itself never writes to the read only mapping, since that faults on a Vita.
//
// On a PS Vita TV on firmware 3.65 every kernel wait but the message pipe receive and send does
// what it waits for and still fails with 0x80022005 (INVALID_MEMORY_ACCESS), writing nothing to the
// timeout or past it. The waits that can end at once take the count, the mutex lock, the bits with
// their clear, the read lock, the thread's exit status and the auto reset event's bit, and the wait
// on several events fails the same way. The message pipe receive and send work, move the message
// and write nothing. The blocking semaphore waits end by themselves, after the timeout or the
// signal, whose count the waiter takes, with INVALID_MEMORY_ACCESS and not a timeout. The condition
// wait that times out gives the same and holds the mutex again after. The lightweight mutex lock
// that can end at once works and writes nothing.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define MARKER 0xA5A5A5A5
#define WORDS 16
#define GIVEN 1000000
#define SHORT 10000
#define LONG 5000000

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static volatile SceUInt32 *writable;
static SceUInt *timeout;
static SceUID object, cond_mutex;
static volatile int wait_result, holds_mutex;
static SceKernelLwMutexWork lw_mutex;

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// Sets the timeout through the writable mapping, and the words after it to the marker
static void fill(SceUInt32 value) {
    writable[0] = value;
    for (int i = 1; i < WORDS; i++)
        writable[i] = MARKER;
}

// Whether any word changed from what fill set
static int changed(SceUInt32 value) {
    int any = writable[0] != value;
    for (int i = 1; i < WORDS; i++)
        any |= writable[i] != MARKER;
    return any;
}

static int returns_at_once(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static int sema_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    wait_result = sceKernelWaitSema(object, 1, timeout);
    return 0;
}

static int cond_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelLockMutex(cond_mutex, 1, NULL);
    wait_result = sceKernelWaitCond(object, timeout);
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(cond_mutex, (SceKernelMutexInfo *)&info);
    holds_mutex = info.currentOwnerId == sceKernelGetThreadId() && info.currentCount == 1;
    if (holds_mutex)
        sceKernelUnlockMutex(cond_mutex, 1);
    return 0;
}

static void print_wait(const char *name, int result, SceUInt32 value) {
    PRINTF(result, "%s", name);
    PRINTF(changed(value), "%s: the words changed", name);
}

static int pipe_bytes(SceUID pipe) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info.bufSize - info.freeSize;
}

static void at_once_cases(void) {
    const SceUID sema = sceKernelCreateSema("ro timeouts", 0, 1, 1, NULL);
    fill(GIVEN);
    print_wait("semaphore wait that can end at once", sceKernelWaitSema(sema, 1, timeout), GIVEN);
    SceKernelSemaInfo sema_info = { .size = sizeof(sema_info) };
    sceKernelGetSemaInfo(sema, &sema_info);
    PRINT("semaphore wait that can end at once: the count after", sema_info.currentCount);
    sceKernelDeleteSema(sema);

    const SceUID mutex = sceKernelCreateMutex("ro timeouts", 0, 0, NULL);
    fill(GIVEN);
    print_wait("mutex lock that can end at once", sceKernelLockMutex(mutex, 1, timeout), GIVEN);
    MutexInfo mutex_info = { .size = sizeof(mutex_info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&mutex_info);
    PRINT("mutex lock that can end at once: its lock count after", mutex_info.currentCount);
    if (mutex_info.currentCount > 0)
        sceKernelUnlockMutex(mutex, mutex_info.currentCount);
    sceKernelDeleteMutex(mutex);

    const SceUID flag = sceKernelCreateEventFlag("ro timeouts", 0, 0x3, NULL);
    unsigned int bits = 0;
    fill(GIVEN);
    print_wait("event flag wait with clear that can end at once",
        sceKernelWaitEventFlag(flag, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &bits, timeout),
        GIVEN);
    SceKernelEventFlagInfo flag_info = { .size = sizeof(flag_info) };
    sceKernelGetEventFlagInfo(flag, &flag_info);
    PRINT("event flag wait with clear that can end at once: the pattern after",
        flag_info.currentPattern);
    PRINT("event flag wait with clear that can end at once: the bits it wrote", bits);
    sceKernelDeleteEventFlag(flag);

    char message[8] = "message";
    SceUID pipe = sceKernelCreateMsgPipe("ro timeouts", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    sceKernelTrySendMsgPipe(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    fill(GIVEN);
    print_wait("message pipe receive that can end at once", sceKernelReceiveMsgPipe(pipe,
        message, sizeof(message), MSG_PIPE_MODE_FULL, NULL, timeout), GIVEN);
    PRINT("message pipe receive that can end at once: the bytes left in the pipe",
        pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("ro timeouts", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    fill(GIVEN);
    print_wait("message pipe send that can end at once", sceKernelSendMsgPipe(pipe, message,
        sizeof(message), MSG_PIPE_MODE_FULL, NULL, timeout), GIVEN);
    PRINT("message pipe send that can end at once: the bytes in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);

    const SceUID rwlock = sceKernelCreateRWLock("ro timeouts", 0, NULL);
    fill(GIVEN);
    print_wait("read lock that can end at once", sceKernelLockReadRWLock(rwlock, timeout), GIVEN);
    SceKernelRWLockInfo rwlock_info = { .size = sizeof(rwlock_info) };
    sceKernelGetRWLockInfo(rwlock, &rwlock_info);
    PRINT("read lock that can end at once: its lock count after", rwlock_info.lockCount);
    if (rwlock_info.lockCount > 0)
        sceKernelUnlockReadRWLock(rwlock);
    sceKernelDeleteRWLock(rwlock);

    const SceUID thread = test_start_thread(returns_at_once);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    int status = 0xDEAD;
    fill(GIVEN);
    print_wait("thread end wait that can end at once",
        sceKernelWaitThreadEnd(thread, &status, timeout), GIVEN);
    PRINT("thread end wait that can end at once: the status it wrote", status);
    sceKernelDeleteThread(thread);

    const SceUID event
        = sceKernelCreateSimpleEvent("ro timeouts", EVENT_ATTR_AUTO_RESET, EVENT_BIT_A, NULL);
    SceUInt32 pattern = 0;
    SceUInt64 user_data = 0;
    fill(GIVEN);
    print_wait("auto reset event wait that can end at once",
        sceKernelWaitEvent(event, EVENT_BIT_A, &pattern, &user_data, timeout), GIVEN);
    SceUInt32 after = 0;
    sceKernelGetEventPattern(event, &after);
    PRINT("auto reset event wait that can end at once: its pattern after", after);
    sceKernelDeleteSimpleEvent(event);

    const SceUID set = sceKernelCreateSimpleEvent("ro timeouts set", 0, EVENT_BIT_A, NULL);
    const SceUID unset = sceKernelCreateSimpleEvent("ro timeouts unset", 0, 0, NULL);
    WaitEvent events[2] = { { set, EVENT_BIT_A }, { unset, EVENT_BIT_A } };
    ResultEvent results[2];
    fill(GIVEN);
    print_wait("wait on several events that can end at once",
        sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, results, timeout), GIVEN);
    sceKernelDeleteSimpleEvent(set);
    sceKernelDeleteSimpleEvent(unset);
}

// Waits 1 s for the thread to end, deleting the object with end_object if it hasn't, then prints
// whether it ended by itself and deletes the thread
static void join_or_delete(const char *name, SceUID thread, int (*end_object)(SceUID)) {
    SceUInt join = 1000000;
    const int by_itself = sceKernelWaitThreadEnd(thread, NULL, &join) == 0;
    if (!by_itself) {
        end_object(object);
        object = -1;
        sceKernelWaitThreadEnd(thread, NULL, NULL);
    }
    sceKernelDeleteThread(thread);
    PRINTF(by_itself, "%s: it ended by itself", name);
}

static void blocking_cases(void) {
    const char *name = "semaphore wait that times out";
    object = sceKernelCreateSema("ro timeouts", 0, 0, 1, NULL);
    fill(SHORT);
    SceUID thread = test_start_thread(sema_waiter);
    join_or_delete(name, thread, sceKernelDeleteSema);
    print_wait(name, wait_result, SHORT);
    if (object >= 0)
        sceKernelDeleteSema(object);

    name = "semaphore wait woken by a signal";
    object = sceKernelCreateSema("ro timeouts", 0, 0, 1, NULL);
    fill(LONG);
    thread = test_start_thread(sema_waiter);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, object));
    sceKernelSignalSema(object, 1);
    join_or_delete(name, thread, sceKernelDeleteSema);
    print_wait(name, wait_result, LONG);
    if (object >= 0) {
        SceKernelSemaInfo info = { .size = sizeof(info) };
        sceKernelGetSemaInfo(object, &info);
        PRINTF(info.currentCount, "%s: the count after", name);
        sceKernelDeleteSema(object);
    }

    name = "condition wait that times out";
    cond_mutex = sceKernelCreateMutex("ro timeouts cond", 0, 0, NULL);
    object = sceKernelCreateCond("ro timeouts", 0, cond_mutex, NULL);
    holds_mutex = 0;
    fill(SHORT);
    thread = test_start_thread(cond_waiter);
    join_or_delete(name, thread, sceKernelDeleteCond);
    print_wait(name, wait_result, SHORT);
    PRINTF(holds_mutex, "%s: the waiter holds the mutex after", name);
    if (object >= 0)
        sceKernelDeleteCond(object);
    sceKernelDeleteMutex(cond_mutex);
}

int main(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock("ro timeouts", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("allocate the original", test_created(original));
    const SceUID opened = original >= 0 ? sceKernelOpenMemBlock("ro timeouts", 0x10) : original;
    PRINT("open it read only", test_created(opened));
    if (opened < 0) {
        if (original >= 0)
            sceKernelFreeMemBlock(original);
        test_exit();
        return 0;
    }
    writable = base_of(original);
    timeout = (SceUInt *)base_of(opened);

    at_once_cases();
    blocking_cases();

    // Last, in case the lightweight mutex code writes the timeout itself, which faults
    sceKernelCreateLwMutex(&lw_mutex, "ro timeouts", 0, 0, NULL);
    fill(GIVEN);
    print_wait("lightweight mutex lock that can end at once",
        sceKernelLockLwMutex(&lw_mutex, 1, timeout), GIVEN);
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    PRINT("lightweight mutex lock that can end at once: its lock count after", info.currentCount);
    if (info.currentCount > 0)
        sceKernelUnlockLwMutex(&lw_mutex, info.currentCount);
    sceKernelDeleteLwMutex(&lw_mutex);

    sceKernelCloseMemBlock(opened);
    sceKernelFreeMemBlock(original);
    test_exit();
    return 0;
}
