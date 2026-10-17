// Thread, sync and memory block calls that read something the app passes them, given an address
// that no memory block maps for it. The address is the base of a 64 KiB USER_RW block freed right
// before the call, which is then in no block, as the test checks first with
// sceKernelFindMemBlockByAddr. Nothing that could make a block runs between the free and the
// call, so objects that need a thread are made before. Each case prints the result, deletes what
// a create made anyway, and for a call that changes its object, what it did. The cases:
// - the name sceKernelAllocMemBlock and sceKernelOpenMemBlock take, and the option struct of the
//   allocation
// - the name of a new semaphore, mutex, event flag, condition variable, reader/writer lock,
//   message pipe, thread, timer, simple event and callback, and the name sceKernelOpenSema takes
// - the option struct of a new semaphore, mutex, event flag, condition variable, reader/writer
//   lock, message pipe and thread
// - the message of a message pipe send and try send, and the vector of a vector send, with the
//   bytes in the pipe after
// - the arguments of sceKernelStartThread, given with a size of 8, and whether the thread ran,
//   and a thread created with its entry there, which is deleted without being started
// - the time sceKernelSetTimerTime sets, the interval sceKernelSetTimerEvent sets, and the events
//   a wait on several events waits for
// - last, timeouts there, for waits that can end at once: a semaphore with its count taken or
//   not after, a mutex, an event flag with clear, a message pipe receive and a thread end. Then a
//   semaphore wait that can't end at once, on a thread of its own. The test waits 1 s for that
//   thread to end, and if it hasn't, it deletes the semaphore so the thread ends either way, and
//   prints whether that was needed. Then a lightweight mutex that can be locked at once, and the
//   name of a new lightweight mutex, in case its library code reads them itself, which faults.
// The app itself never touches the address.
//
// On a PS Vita TV on firmware 3.65 the address is in no block, with 0x80022007 (VA2PA_FAULT), and
// every kernel call given it fails with 0x80022005 (INVALID_MEMORY_ACCESS). No object or block is
// made or opened, no thread starts, no message reaches the pipe and the sends write no size. Unlike
// a timeout in read only memory in readonly_timeouts, a timeout there leaves the object as it was,
// so the semaphore's count, the mutex, the event flag's bits and the pipe's message are all still
// there, and the semaphore wait that can't end at once ends by itself with the same error. A thread
// with its entry there isn't made either, with 0x80020006 (ILLEGAL_ADDR). The lightweight mutex
// lock with its timeout there works and takes the lock, while a lightweight mutex with its name
// there fails with INVALID_MEMORY_ACCESS.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400

static SceUID sema;
static volatile int wait_result, ran;
static SceUInt *wait_timeout;
static SceKernelLwMutexWork lw_mutex;

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    ran = 1;
    return 0;
}

static int callback(int notify_id, int notify_count, int notify_arg, void *common) {
    (void)notify_id;
    (void)notify_count;
    (void)notify_arg;
    (void)common;
    return 0;
}

static int sema_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    wait_result = sceKernelWaitSema(sema, 1, wait_timeout);
    return 0;
}

// Allocates a block and frees it, and returns its base, which is in no block until the next
// allocation
static void *unmapped(void) {
    const SceUID block
        = sceKernelAllocMemBlock("unmapped in", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    sceKernelFreeMemBlock(block);
    return base;
}

static int pipe_bytes(SceUID pipe) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info.bufSize - info.freeSize;
}

static void memory_block_cases(void) {
    SceUID block = sceKernelAllocMemBlock(unmapped(), SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB,
        NULL);
    PRINT("allocate a block with its name there", test_created(block));
    if (block >= 0)
        sceKernelFreeMemBlock(block);
    block = sceKernelAllocMemBlock("unmapped in", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB,
        unmapped());
    PRINT("allocate a block with its option struct there", test_created(block));
    if (block >= 0)
        sceKernelFreeMemBlock(block);
    const SceUID opened = sceKernelOpenMemBlock(unmapped(), 0x10);
    PRINT("open a block by a name there", test_created(opened));
    if (opened >= 0)
        sceKernelCloseMemBlock(opened);
}

static void print_create(const char *what, SceUID id, int (*end)(SceUID)) {
    PRINTF(test_created(id), "%s", what);
    if (id >= 0)
        end(id);
}

static void name_cases(void) {
    const int prio = SCE_KERNEL_DEFAULT_PRIORITY_USER;
    print_create("semaphore: create with the name there",
        sceKernelCreateSema(unmapped(), 0, 0, 1, NULL), sceKernelDeleteSema);
    print_create("mutex: create with the name there",
        sceKernelCreateMutex(unmapped(), 0, 0, NULL), sceKernelDeleteMutex);
    print_create("event flag: create with the name there",
        sceKernelCreateEventFlag(unmapped(), 0, 0, NULL), sceKernelDeleteEventFlag);
    const SceUID mutex = sceKernelCreateMutex("unmapped in cond", 0, 0, NULL);
    print_create("condition variable: create with the name there",
        sceKernelCreateCond(unmapped(), 0, mutex, NULL), sceKernelDeleteCond);
    print_create("reader/writer lock: create with the name there",
        sceKernelCreateRWLock(unmapped(), 0, NULL), sceKernelDeleteRWLock);
    print_create("message pipe: create with the name there",
        sceKernelCreateMsgPipe(unmapped(), MSG_PIPE_TYPE_USER, 0, 16, NULL),
        sceKernelDeleteMsgPipe);
    print_create("thread: create with the name there",
        sceKernelCreateThread(unmapped(), entry, prio, 0x4000, 0, 0, NULL), sceKernelDeleteThread);
    print_create("timer: create with the name there", sceKernelCreateTimer(unmapped(), 0, NULL),
        sceKernelDeleteTimer);
    print_create("simple event: create with the name there",
        sceKernelCreateSimpleEvent(unmapped(), 0, 0, NULL), sceKernelDeleteSimpleEvent);
    print_create("callback: create with the name there",
        sceKernelCreateCallback(unmapped(), 0, callback, NULL), sceKernelDeleteCallback);
    const SceUID opened = sceKernelOpenSema(unmapped());
    PRINT("semaphore: open by a name there", test_created(opened));
    if (opened >= 0)
        sceKernelCloseSema(opened);

    print_create("semaphore: create with the option struct there",
        sceKernelCreateSema("unmapped in", 0, 0, 1, unmapped()), sceKernelDeleteSema);
    print_create("mutex: create with the option struct there",
        sceKernelCreateMutex("unmapped in", 0, 0, unmapped()), sceKernelDeleteMutex);
    print_create("event flag: create with the option struct there",
        sceKernelCreateEventFlag("unmapped in", 0, 0, unmapped()), sceKernelDeleteEventFlag);
    print_create("condition variable: create with the option struct there",
        sceKernelCreateCond("unmapped in", 0, mutex, unmapped()), sceKernelDeleteCond);
    print_create("reader/writer lock: create with the option struct there",
        sceKernelCreateRWLock("unmapped in", 0, unmapped()), sceKernelDeleteRWLock);
    print_create("message pipe: create with the option struct there",
        sceKernelCreateMsgPipe("unmapped in", MSG_PIPE_TYPE_USER, 0, 16, unmapped()),
        sceKernelDeleteMsgPipe);
    print_create("thread: create with the option struct there",
        sceKernelCreateThread("unmapped in", entry, prio, 0x4000, 0, 0, unmapped()),
        sceKernelDeleteThread);
    sceKernelDeleteMutex(mutex);
}

static void msg_pipe_cases(void) {
    SceSize size = 0xDEAD;
    SceUInt timeout = TEST_WAIT_US;
    SceUID pipe = sceKernelCreateMsgPipe("unmapped in", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    PRINT("send a message from there",
        sceKernelSendMsgPipe(pipe, unmapped(), 8, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("send a message from there: the size it sent", size);
    PRINT("send a message from there: the bytes in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("unmapped in", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    PRINT("try send a message from there",
        sceKernelTrySendMsgPipe(pipe, unmapped(), 8, MSG_PIPE_MODE_FULL, NULL));
    PRINT("try send a message from there: the bytes in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("unmapped in", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    size = 0xDEAD;
    timeout = TEST_WAIT_US;
    PRINT("send a vector from there", sceKernelSendMsgPipeVector(pipe, unmapped(), 1,
        MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("send a vector from there: the size it sent", size);
    PRINT("send a vector from there: the bytes in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);
}

static void thread_cases(void) {
    const int prio = SCE_KERNEL_DEFAULT_PRIORITY_USER;
    const SceUID thread = sceKernelCreateThread("unmapped in", entry, prio, 0x4000, 0, 0, NULL);
    ran = 0;
    const int started = sceKernelStartThread(thread, 8, unmapped());
    PRINT("start a thread with its arguments there", started);
    if (started == 0) {
        SceUInt timeout = TEST_WAIT_US;
        PRINT("start a thread with its arguments there: its end",
            sceKernelWaitThreadEnd(thread, NULL, &timeout));
        PRINT("start a thread with its arguments there: it ran", ran);
    }
    sceKernelDeleteThread(thread);

    const SceUID unstarted
        = sceKernelCreateThread("unmapped in", (SceKernelThreadEntry)(SceUIntPtr)unmapped(), prio,
            0x4000, 0, 0, NULL);
    PRINT("create a thread with its entry there", test_created(unstarted));
    if (unstarted >= 0)
        PRINT("delete it without starting it", sceKernelDeleteThread(unstarted));
}

static void timer_and_event_cases(void) {
    const SceUID timer = sceKernelCreateTimer("unmapped in", 0, NULL);
    PRINT("set the timer's time from there", sceKernelSetTimerTime(timer, unmapped()));
    PRINT("set a timer event with the interval there",
        sceKernelSetTimerEvent(timer, TIMER_TYPE_SET_EVENT, unmapped(), 0));
    sceKernelDeleteTimer(timer);

    ResultEvent results[2];
    SceUInt timeout = TEST_WAIT_US;
    PRINT("wait on several events listed there",
        sceKernelWaitMultipleEvents(unmapped(), 2, EVENT_WAIT_MODE_OR, results, &timeout));
}

static void timeout_cases(void) {
    SceUID id = sceKernelCreateSema("unmapped in", 0, 1, 1, NULL);
    PRINT("semaphore wait that can end at once, the timeout there",
        sceKernelWaitSema(id, 1, unmapped()));
    SceKernelSemaInfo sema_info = { .size = sizeof(sema_info) };
    sceKernelGetSemaInfo(id, &sema_info);
    PRINT("semaphore wait that can end at once, the timeout there: the count after",
        sema_info.currentCount);
    sceKernelDeleteSema(id);

    id = sceKernelCreateMutex("unmapped in", 0, 0, NULL);
    PRINT("mutex lock that can end at once, the timeout there",
        sceKernelLockMutex(id, 1, unmapped()));
    MutexInfo mutex_info = { .size = sizeof(mutex_info) };
    sceKernelGetMutexInfo(id, (SceKernelMutexInfo *)&mutex_info);
    PRINT("mutex lock that can end at once, the timeout there: its lock count after",
        mutex_info.currentCount);
    if (mutex_info.currentCount > 0)
        sceKernelUnlockMutex(id, mutex_info.currentCount);
    sceKernelDeleteMutex(id);

    id = sceKernelCreateEventFlag("unmapped in", 0, 0x3, NULL);
    unsigned int bits = 0;
    PRINT("event flag wait with clear that can end at once, the timeout there",
        sceKernelWaitEventFlag(id, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &bits, unmapped()));
    SceKernelEventFlagInfo flag_info = { .size = sizeof(flag_info) };
    sceKernelGetEventFlagInfo(id, &flag_info);
    PRINT("event flag wait with clear that can end at once, the timeout there: the pattern after",
        flag_info.currentPattern);
    sceKernelDeleteEventFlag(id);

    char message[8] = "message";
    id = sceKernelCreateMsgPipe("unmapped in", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    sceKernelTrySendMsgPipe(id, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    PRINT("message pipe receive that can end at once, the timeout there",
        sceKernelReceiveMsgPipe(id, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL,
            unmapped()));
    PRINT("message pipe receive that can end at once, the timeout there: the bytes left",
        pipe_bytes(id));
    sceKernelDeleteMsgPipe(id);

    id = test_start_thread(entry);
    sceKernelWaitThreadEnd(id, NULL, NULL);
    PRINT("thread end wait that can end at once, the timeout there",
        sceKernelWaitThreadEnd(id, NULL, unmapped()));
    sceKernelDeleteThread(id);

    // A wait that can't end at once, which a wrong implementation may never end
    sema = sceKernelCreateSema("unmapped in", 0, 0, 1, NULL);
    const SceUID waiter = sceKernelCreateThread(
        "unmapped in waiter", sema_waiter, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    wait_timeout = unmapped();
    sceKernelStartThread(waiter, 0, NULL);
    SceUInt join = 1000000;
    const int by_itself = sceKernelWaitThreadEnd(waiter, NULL, &join) == 0;
    if (!by_itself) {
        sceKernelDeleteSema(sema);
        sema = -1;
        sceKernelWaitThreadEnd(waiter, NULL, NULL);
    }
    sceKernelDeleteThread(waiter);
    PRINT("semaphore wait that can't end at once, the timeout there: it ended by itself",
        by_itself);
    PRINT("semaphore wait that can't end at once, the timeout there", wait_result);
    if (sema >= 0)
        sceKernelDeleteSema(sema);
}

int main(void) {
    PRINT("find the base of a freed block", sceKernelFindMemBlockByAddr(unmapped(), 0));

    memory_block_cases();
    name_cases();
    msg_pipe_cases();
    thread_cases();
    timer_and_event_cases();
    timeout_cases();

    // Last, in case the lightweight mutex code reads the timeout or the name itself, which faults
    sceKernelCreateLwMutex(&lw_mutex, "unmapped in", 0, 0, NULL);
    PRINT("lightweight mutex lock that can end at once, the timeout there",
        sceKernelLockLwMutex(&lw_mutex, 1, unmapped()));
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    PRINT("lightweight mutex lock that can end at once, the timeout there: its lock count after",
        info.currentCount);
    if (info.currentCount > 0)
        sceKernelUnlockLwMutex(&lw_mutex, info.currentCount);
    sceKernelDeleteLwMutex(&lw_mutex);
    const int result = sceKernelCreateLwMutex(&lw_mutex, unmapped(), 0, 0, NULL);
    PRINT("lightweight mutex: create with the name there", result);
    if (result >= 0)
        sceKernelDeleteLwMutex(&lw_mutex);

    test_exit();
    return 0;
}
