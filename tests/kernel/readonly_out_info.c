// The info calls of threads and sync objects given a struct in memory the app can only read, as
// memblock_readonly_out does for the memory block calls. The memory is a block opened with
// sceKernelOpenMemBlock with flags 0x10, which maps the memory of a 64 KiB USER_RW block with
// ENABLE_OPEN read only, so the test sets the struct through the original, its size word to the
// size of the struct and every other word to a marker, and sees through it what a call wrote. For
// a thread, a semaphore, a mutex, an event flag, a condition variable, a reader/writer lock, a
// message pipe, a callback, a simple event, a timer, a lightweight mutex and a lightweight
// condition variable, each made fresh for its case, it prints the result of the info call, whether
// it changed any of the first 64 words, and whether the object still works after: the same info
// call into writable memory, then calls that use the object. Then sceKernelGetThreadRunStatus and
// sceKernelGetSystemInfo, and a semaphore's info call with a size word of 0 and of 4, to see
// whether the kernel checks the memory when it has nothing or only the size word to write. The app
// itself never writes to the read only mapping, since that faults on a Vita.
//
// On a PS Vita TV on firmware 3.65 the info call of every kind fails with 0x80022005
// (INVALID_MEMORY_ACCESS) and writes nothing, and the object works as before after it.
// sceKernelGetSystemInfo fails the same way, and sceKernelGetThreadRunStatus fails with 0x80020006
// (ILLEGAL_ADDR), both writing nothing. A semaphore's info call with a size word of 0 works and
// writes nothing, while one with a size word of 4, which leaves only the size word to write, fails
// with INVALID_MEMORY_ACCESS, like the memory block calls in memblock_readonly_out.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define MARKER 0xA5A5A5A5
#define WORDS 64

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

enum Kind {
    THREAD,
    SEMA,
    MUTEX,
    EVENT_FLAG,
    COND,
    RWLOCK,
    MSG_PIPE,
    CALLBACK,
    SIMPLE_EVENT,
    TIMER,
    LW_MUTEX,
    LW_COND,
    KIND_COUNT
};

static const char *const kind_names[] = {
    "thread", "semaphore", "mutex", "event flag", "condition variable", "reader/writer lock",
    "message pipe", "callback", "simple event", "timer", "lightweight mutex",
    "lightweight condition variable",
};

static const SceSize info_sizes[] = {
    sizeof(SceKernelThreadInfo), sizeof(SceKernelSemaInfo), sizeof(MutexInfo),
    sizeof(SceKernelEventFlagInfo), sizeof(SceKernelCondInfo), sizeof(SceKernelRWLockInfo),
    sizeof(SceKernelMppInfo), sizeof(CallbackInfo), sizeof(EventInfo), sizeof(TimerInfo),
    sizeof(LwMutexInfo), sizeof(LwCondInfo),
};

static volatile SceUInt32 *writable;
static void *readable;
static SceUInt32 spare[WORDS];
static SceUID cond_mutex;
static SceKernelLwMutexWork lw_mutex, lw_cond_mutex;
static SceKernelLwCondWork lw_cond;

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static int callback(int notify_id, int notify_count, int notify_arg, void *common) {
    (void)notify_id;
    (void)notify_count;
    (void)notify_arg;
    (void)common;
    return 0;
}

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// Sets the words through the writable mapping, the first to first and the rest to the marker
static void fill(SceUInt32 first) {
    writable[0] = first;
    for (int i = 1; i < WORDS; i++)
        writable[i] = MARKER;
}

// Whether any word changed from what fill set
static int changed(SceUInt32 first) {
    int any = writable[0] != first;
    for (int i = 1; i < WORDS; i++)
        any |= writable[i] != MARKER;
    return any;
}

// Makes an object of the kind and returns its UID
static SceUID make(int kind) {
    const int prio = SCE_KERNEL_DEFAULT_PRIORITY_USER;
    switch (kind) {
    case THREAD: return sceKernelCreateThread("ro info", entry, prio, 0x4000, 0, 0, NULL);
    case SEMA: return sceKernelCreateSema("ro info", 0, 0, 1, NULL);
    case MUTEX: return sceKernelCreateMutex("ro info", 0, 0, NULL);
    case EVENT_FLAG: return sceKernelCreateEventFlag("ro info", 0, 0, NULL);
    case COND:
        cond_mutex = sceKernelCreateMutex("ro info cond", 0, 0, NULL);
        return sceKernelCreateCond("ro info", 0, cond_mutex, NULL);
    case RWLOCK: return sceKernelCreateRWLock("ro info", 0, NULL);
    case MSG_PIPE: return sceKernelCreateMsgPipe("ro info", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    case CALLBACK: return sceKernelCreateCallback("ro info", 0, callback, NULL);
    case SIMPLE_EVENT: return sceKernelCreateSimpleEvent("ro info", 0, 0, NULL);
    case TIMER: return sceKernelCreateTimer("ro info", 0, NULL);
    case LW_MUTEX: {
        const int result = sceKernelCreateLwMutex(&lw_mutex, "ro info", 0, 0, NULL);
        LwMutexInfo info = { .size = sizeof(info) };
        if (result < 0 || sceKernelGetLwMutexInfo(&lw_mutex, &info) < 0)
            return result < 0 ? result : -1;
        return info.uid;
    }
    case LW_COND: {
        sceKernelCreateLwMutex(&lw_cond_mutex, "ro info cond", 0, 0, NULL);
        const int result = sceKernelCreateLwCond(&lw_cond, "ro info", 0, &lw_cond_mutex, NULL);
        LwCondInfo info = { .size = sizeof(info) };
        if (result < 0 || sceKernelGetLwCondInfo(&lw_cond, &info) < 0)
            return result < 0 ? result : -1;
        return info.uid;
    }
    }
    return -1;
}

static int get_info(int kind, SceUID id, void *info) {
    switch (kind) {
    case THREAD: return sceKernelGetThreadInfo(id, info);
    case SEMA: return sceKernelGetSemaInfo(id, info);
    case MUTEX: return sceKernelGetMutexInfo(id, info);
    case EVENT_FLAG: return sceKernelGetEventFlagInfo(id, info);
    case COND: return sceKernelGetCondInfo(id, info);
    case RWLOCK: return sceKernelGetRWLockInfo(id, info);
    case MSG_PIPE: return sceKernelGetMsgPipeInfo(id, info);
    case CALLBACK: return sceKernelGetCallbackInfo(id, info);
    case SIMPLE_EVENT: return sceKernelGetEventInfo(id, info);
    case TIMER: return sceKernelGetTimerInfo(id, info);
    case LW_MUTEX: return sceKernelGetLwMutexInfoById(id, info);
    case LW_COND: return sceKernelGetLwCondInfoById(id, info);
    }
    return -1;
}

// Returns the first of two results that is an error, or 0
static int both(int first, int second) {
    return first < 0 ? first : second < 0 ? second : 0;
}

// Uses the object with calls that leave it as it was, and returns 0 or the first error
static int use(int kind, SceUID id) {
    char message[4] = { 1, 2, 3, 4 };
    SceUInt timeout = TEST_WAIT_US;
    // Each pair runs in its order, the first call's result kept before the second runs
    int first = 0;
    switch (kind) {
    case THREAD:
        first = sceKernelStartThread(id, 0, NULL);
        return both(first, sceKernelWaitThreadEnd(id, NULL, &timeout));
    case SEMA:
        first = sceKernelSignalSema(id, 1);
        return both(first, sceKernelPollSema(id, 1));
    case MUTEX:
        first = sceKernelLockMutex(id, 1, NULL);
        return both(first, sceKernelUnlockMutex(id, 1));
    case EVENT_FLAG:
        first = sceKernelSetEventFlag(id, 1);
        return both(first,
            sceKernelPollEventFlag(id, 1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, NULL));
    case COND: return sceKernelSignalCond(id);
    case RWLOCK:
        first = sceKernelLockWriteRWLock(id, NULL);
        return both(first, sceKernelUnlockWriteRWLock(id));
    case MSG_PIPE:
        first = sceKernelTrySendMsgPipe(id, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
        return both(first,
            sceKernelTryReceiveMsgPipe(id, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL));
    case CALLBACK:
        first = sceKernelNotifyCallback(id, 1);
        return both(first, sceKernelCancelCallback(id));
    case SIMPLE_EVENT:
        first = sceKernelSetEvent(id, EVENT_BIT_A, 0);
        return both(first, sceKernelPollEvent(id, EVENT_BIT_A, NULL, NULL));
    case TIMER:
        first = sceKernelStartTimer(id);
        return both(first, sceKernelStopTimer(id));
    case LW_MUTEX:
        first = sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        return both(first, sceKernelUnlockLwMutex(&lw_mutex, 1));
    case LW_COND: return sceKernelSignalLwCond(&lw_cond);
    }
    return -1;
}

static void destroy(int kind, SceUID id) {
    switch (kind) {
    case THREAD: sceKernelDeleteThread(id); break;
    case SEMA: sceKernelDeleteSema(id); break;
    case MUTEX: sceKernelDeleteMutex(id); break;
    case EVENT_FLAG: sceKernelDeleteEventFlag(id); break;
    case COND:
        sceKernelDeleteCond(id);
        sceKernelDeleteMutex(cond_mutex);
        break;
    case RWLOCK: sceKernelDeleteRWLock(id); break;
    case MSG_PIPE: sceKernelDeleteMsgPipe(id); break;
    case CALLBACK: sceKernelDeleteCallback(id); break;
    case SIMPLE_EVENT: sceKernelDeleteSimpleEvent(id); break;
    case TIMER: sceKernelDeleteTimer(id); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case LW_COND:
        sceKernelDeleteLwCond(&lw_cond);
        sceKernelDeleteLwMutex(&lw_cond_mutex);
        break;
    }
}

static void info_case(int kind) {
    const char *name = kind_names[kind];
    const SceUID id = make(kind);
    PRINTF(test_created(id), "%s: make it", name);
    if (id < 0)
        return;
    fill(info_sizes[kind]);
    PRINTF(get_info(kind, id, readable), "%s: info into it", name);
    PRINTF(changed(info_sizes[kind]), "%s: info into it: the words changed", name);
    spare[0] = info_sizes[kind];
    const int again = get_info(kind, id, spare);
    PRINTF(both(again, use(kind, id)), "%s: it still works after", name);
    destroy(kind, id);
}

int main(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock("ro info", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("allocate the original", test_created(original));
    const SceUID opened = original >= 0 ? sceKernelOpenMemBlock("ro info", 0x10) : original;
    PRINT("open it read only", test_created(opened));
    if (opened < 0) {
        if (original >= 0)
            sceKernelFreeMemBlock(original);
        test_exit();
        return 0;
    }
    writable = base_of(original);
    readable = (void *)base_of(opened);

    for (int kind = 0; kind < KIND_COUNT; kind++)
        info_case(kind);

    fill(sizeof(SceKernelThreadRunStatus));
    PRINT("run status into it", sceKernelGetThreadRunStatus(0, readable));
    PRINT("run status into it: the words changed", changed(sizeof(SceKernelThreadRunStatus)));
    fill(sizeof(SceKernelSystemInfo));
    PRINT("system info into it", sceKernelGetSystemInfo(readable));
    PRINT("system info into it: the words changed", changed(sizeof(SceKernelSystemInfo)));

    // Sizes that leave the kernel nothing or only the size word to write
    const SceUID sema = sceKernelCreateSema("ro info sizes", 0, 0, 1, NULL);
    static const SceSize sizes[] = { 0, 4 };
    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fill(sizes[i]);
        PRINTF(sceKernelGetSemaInfo(sema, readable), "semaphore, size 0x%X: info into it",
            (unsigned)sizes[i]);
        PRINTF(changed(sizes[i]), "semaphore, size 0x%X: info into it: the words changed",
            (unsigned)sizes[i]);
    }
    sceKernelDeleteSema(sema);

    sceKernelCloseMemBlock(opened);
    sceKernelFreeMemBlock(original);
    test_exit();
    return 0;
}
