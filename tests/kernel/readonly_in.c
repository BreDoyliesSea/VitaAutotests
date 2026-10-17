// Calls that only read what the app passes them, given it in memory the app can only read. The
// memory is a block opened with sceKernelOpenMemBlock with flags 0x10, which maps the memory of a
// 64 KiB USER_RW block with ENABLE_OPEN read only, so the test writes what a call reads through
// the original. The cases, each on fresh objects:
// - the name of a new semaphore, mutex, event flag, condition variable, reader/writer lock,
//   message pipe, thread, timer, simple event, callback and lightweight mutex: the create and
//   whether its info has the name. Then the name sceKernelOpenSema takes, for an openable one.
// - the option struct of a new semaphore, of a new mutex with a ceiling, whose info should report
//   the ceiling in the struct, and of a new thread
// - the message of a message pipe send and try send, and the vector of a vector send along with
//   its message, and whether a receive gets the message after
// - the arguments sceKernelStartThread copies for a thread, and whether the thread got them
// - the time sceKernelSetTimerTime sets and the interval sceKernelSetTimerEvent sets
// - the events a wait on several events waits for, and whether it reports the one that was set
// - the name sceKernelAllocMemBlock and sceKernelOpenMemBlock take, and the option struct of the
//   allocation, which asks for ENABLE_OPEN, so the open shows whether the call read it
// The app itself never writes to the read only mapping, since that faults on a Vita.
//
// On a PS Vita TV on firmware 3.65 every call reads what it is given in the read only mapping.
// Every kind of object is made with its name there and its info has the name, and the open by a
// name there works. The option structs are read, the mutex reporting the ceiling 0x80 from its
// struct. The three sends work and a receive after each gets the message, the thread gets its 8
// bytes of arguments, the timer event takes its interval, and the wait on several events reports
// the set one. The block is made with its name and option struct there, and an open by that name
// works, so the allocation read ENABLE_OPEN. The one call that fails is sceKernelSetTimerTime, with
// 0x80022005 (INVALID_MEMORY_ACCESS), and the timer's time is set to the value it was given anyway.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define CEILING 0x80
#define NAME "ro in name"
#define NAME_OFFSET 0x100
#define OPT_OFFSET 0x200
#define MESSAGE_OFFSET 0x300
#define VECTOR_OFFSET 0x400
#define CLOCK_OFFSET 0x500
#define EVENTS_OFFSET 0x600
#define BLOCK_NAME "ro in block"

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

enum Kind {
    SEMA,
    MUTEX,
    EVENT_FLAG,
    COND,
    RWLOCK,
    MSG_PIPE,
    THREAD,
    TIMER,
    SIMPLE_EVENT,
    CALLBACK,
    LW_MUTEX,
    KIND_COUNT
};

static const char *const kind_names[] = {
    "semaphore", "mutex", "event flag", "condition variable", "reader/writer lock",
    "message pipe", "thread", "timer", "simple event", "callback", "lightweight mutex",
};

static volatile char *writable;
static char *readable;
static SceUID cond_mutex;
static SceKernelLwMutexWork lw_mutex;
static char got_args[8];
static volatile SceSize got_size;

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static int copies_args(SceSize args, void *argp) {
    got_size = args;
    if (args == sizeof(got_args))
        sceClibMemcpy(got_args, argp, sizeof(got_args));
    return 0;
}

static int callback(int notify_id, int notify_count, int notify_arg, void *common) {
    (void)notify_id;
    (void)notify_count;
    (void)notify_arg;
    (void)common;
    return 0;
}

static void *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// Copies size bytes from source into the window at offset, through the writable mapping, and
// returns where the read only mapping shows them
static void *put(unsigned int offset, const void *source, SceSize size) {
    for (SceSize i = 0; i < size; i++)
        writable[offset + i] = ((const char *)source)[i];
    return readable + offset;
}

// Makes an object of the kind with the name and returns its UID
static SceUID make(int kind, const char *name) {
    const int prio = SCE_KERNEL_DEFAULT_PRIORITY_USER;
    switch (kind) {
    case SEMA: return sceKernelCreateSema(name, 0, 0, 1, NULL);
    case MUTEX: return sceKernelCreateMutex(name, 0, 0, NULL);
    case EVENT_FLAG: return sceKernelCreateEventFlag(name, 0, 0, NULL);
    case COND:
        cond_mutex = sceKernelCreateMutex("ro in cond", 0, 0, NULL);
        return sceKernelCreateCond(name, 0, cond_mutex, NULL);
    case RWLOCK: return sceKernelCreateRWLock(name, 0, NULL);
    case MSG_PIPE: return sceKernelCreateMsgPipe(name, MSG_PIPE_TYPE_USER, 0, 16, NULL);
    case THREAD: return sceKernelCreateThread(name, entry, prio, 0x4000, 0, 0, NULL);
    case TIMER: return sceKernelCreateTimer(name, 0, NULL);
    case SIMPLE_EVENT: return sceKernelCreateSimpleEvent(name, 0, 0, NULL);
    case CALLBACK: return sceKernelCreateCallback(name, 0, callback, NULL);
    case LW_MUTEX: {
        const int result = sceKernelCreateLwMutex(&lw_mutex, name, 0, 0, NULL);
        LwMutexInfo info = { .size = sizeof(info) };
        if (result < 0 || sceKernelGetLwMutexInfo(&lw_mutex, &info) < 0)
            return result < 0 ? result : -1;
        return info.uid;
    }
    }
    return -1;
}

// Whether the object's info has the name
static int has_name(int kind, SceUID id) {
    // Big enough for any of the info structs, and the name is at the same place in each
    static union {
        SceKernelThreadInfo thread;
        SceKernelSemaInfo sema;
        char bytes[0x100];
    } info;
    sceClibMemset(&info, 0, sizeof(info));
    int result = -1;
    switch (kind) {
    case SEMA:
        info.sema.size = sizeof(SceKernelSemaInfo);
        result = sceKernelGetSemaInfo(id, &info.sema);
        break;
    case MUTEX:
        info.sema.size = sizeof(MutexInfo);
        result = sceKernelGetMutexInfo(id, (SceKernelMutexInfo *)&info);
        break;
    case EVENT_FLAG:
        info.sema.size = sizeof(SceKernelEventFlagInfo);
        result = sceKernelGetEventFlagInfo(id, (SceKernelEventFlagInfo *)&info);
        break;
    case COND:
        info.sema.size = sizeof(SceKernelCondInfo);
        result = sceKernelGetCondInfo(id, (SceKernelCondInfo *)&info);
        break;
    case RWLOCK:
        info.sema.size = sizeof(SceKernelRWLockInfo);
        result = sceKernelGetRWLockInfo(id, (SceKernelRWLockInfo *)&info);
        break;
    case MSG_PIPE:
        info.sema.size = sizeof(SceKernelMppInfo);
        result = sceKernelGetMsgPipeInfo(id, (SceKernelMppInfo *)&info);
        break;
    case THREAD:
        info.thread.size = sizeof(SceKernelThreadInfo);
        result = sceKernelGetThreadInfo(id, &info.thread);
        break;
    case TIMER:
        info.sema.size = sizeof(TimerInfo);
        result = sceKernelGetTimerInfo(id, (TimerInfo *)&info);
        break;
    case SIMPLE_EVENT:
        info.sema.size = sizeof(EventInfo);
        result = sceKernelGetEventInfo(id, (EventInfo *)&info);
        break;
    case CALLBACK:
        info.sema.size = sizeof(CallbackInfo);
        result = sceKernelGetCallbackInfo(id, (SceKernelCallbackInfo *)&info);
        break;
    case LW_MUTEX:
        info.sema.size = sizeof(LwMutexInfo);
        result = sceKernelGetLwMutexInfoById(id, (LwMutexInfo *)&info);
        break;
    }
    return result == 0 && sceClibStrcmp(info.sema.name, NAME) == 0;
}

static void destroy(int kind, SceUID id) {
    switch (kind) {
    case SEMA: sceKernelDeleteSema(id); break;
    case MUTEX: sceKernelDeleteMutex(id); break;
    case EVENT_FLAG: sceKernelDeleteEventFlag(id); break;
    case COND:
        sceKernelDeleteCond(id);
        sceKernelDeleteMutex(cond_mutex);
        break;
    case RWLOCK: sceKernelDeleteRWLock(id); break;
    case MSG_PIPE: sceKernelDeleteMsgPipe(id); break;
    case THREAD: sceKernelDeleteThread(id); break;
    case TIMER: sceKernelDeleteTimer(id); break;
    case SIMPLE_EVENT: sceKernelDeleteSimpleEvent(id); break;
    case CALLBACK: sceKernelDeleteCallback(id); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    }
}

static void name_cases(void) {
    const char *name = put(NAME_OFFSET, NAME, sizeof(NAME));
    for (int kind = 0; kind < KIND_COUNT; kind++) {
        const SceUID id = make(kind, name);
        PRINTF(test_created(id), "%s: create with the name in it", kind_names[kind]);
        if (id < 0)
            continue;
        PRINTF(has_name(kind, id), "%s: its info has the name", kind_names[kind]);
        destroy(kind, id);
    }

    const SceUID openable = sceKernelCreateSema(NAME, SCE_KERNEL_ATTR_OPENABLE, 0, 1, NULL);
    const SceUID opened = sceKernelOpenSema(name);
    PRINT("semaphore: open by the name in it", test_created(opened));
    if (opened >= 0)
        sceKernelCloseSema(opened);
    sceKernelDeleteSema(openable);
}

static void opt_cases(void) {
    const SceKernelSemaOptParam sema_opt = { .size = 4 };
    SceUID id = sceKernelCreateSema("ro in", 0, 0, 1, put(OPT_OFFSET, &sema_opt, sizeof(sema_opt)));
    PRINT("semaphore: create with the option struct in it", test_created(id));
    if (id >= 0)
        sceKernelDeleteSema(id);

    const SceKernelMutexOptParam mutex_opt
        = { .size = sizeof(mutex_opt), .ceilingPriority = CEILING };
    id = sceKernelCreateMutex("ro in", SCE_KERNEL_MUTEX_ATTR_CEILING, 0,
        put(OPT_OFFSET, &mutex_opt, sizeof(mutex_opt)));
    PRINT("mutex with a ceiling: create with the option struct in it", test_created(id));
    if (id >= 0) {
        MutexInfo info = { .size = sizeof(info) };
        sceKernelGetMutexInfo(id, (SceKernelMutexInfo *)&info);
        PRINT("mutex with a ceiling: the ceiling in its info", info.ceilingPriority);
        sceKernelDeleteMutex(id);
    }

    const SceKernelThreadOptParam thread_opt = { .size = 0x14 };
    id = sceKernelCreateThread("ro in", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0,
        put(OPT_OFFSET, &thread_opt, sizeof(thread_opt)));
    PRINT("thread: create with the option struct in it", test_created(id));
    if (id >= 0)
        sceKernelDeleteThread(id);
}

// Receives what the pipe holds and returns whether it's the message
static int receives(SceUID pipe, const char *message, SceSize size) {
    char got[16] = { 0 };
    const int result = sceKernelTryReceiveMsgPipe(pipe, got, size, MSG_PIPE_MODE_FULL, NULL);
    return result == 0 && sceClibMemcmp(got, message, size) == 0;
}

static void msg_pipe_cases(void) {
    static const char message[8] = "message";
    SceSize size = 0xDEAD;
    SceUInt timeout = TEST_WAIT_US;

    SceUID pipe = sceKernelCreateMsgPipe("ro in", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    void *in = put(MESSAGE_OFFSET, message, sizeof(message));
    PRINT("send the message from it",
        sceKernelSendMsgPipe(pipe, in, sizeof(message), MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("send the message from it: the size it sent", size);
    PRINT("send the message from it: a receive gets it", receives(pipe, message, sizeof(message)));
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("ro in", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    PRINT("try send the message from it",
        sceKernelTrySendMsgPipe(pipe, in, sizeof(message), MSG_PIPE_MODE_FULL, NULL));
    PRINT("try send the message from it: a receive gets it",
        receives(pipe, message, sizeof(message)));
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("ro in", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    const MsgPipeVector vector = { .base = in, .size = sizeof(message) };
    const MsgPipeVector *vectors = put(VECTOR_OFFSET, &vector, sizeof(vector));
    size = 0xDEAD;
    timeout = TEST_WAIT_US;
    PRINT("send a vector from it", sceKernelSendMsgPipeVector(pipe, vectors, 1,
        MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("send a vector from it: the size it sent", size);
    PRINT("send a vector from it: a receive gets it", receives(pipe, message, sizeof(message)));
    sceKernelDeleteMsgPipe(pipe);
}

static void thread_args_case(void) {
    static const char args[8] = "args in";
    const SceUID thread = sceKernelCreateThread(
        "ro in args", copies_args, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    PRINT("start a thread with its arguments in it",
        sceKernelStartThread(thread, sizeof(args), put(MESSAGE_OFFSET, args, sizeof(args))));
    SceUInt timeout = TEST_WAIT_US;
    PRINT("start a thread with its arguments in it: its end",
        sceKernelWaitThreadEnd(thread, NULL, &timeout));
    PRINT("start a thread with its arguments in it: the size it got", got_size);
    PRINT("start a thread with its arguments in it: it got them",
        sceClibMemcmp(got_args, args, sizeof(args)) == 0);
    sceKernelDeleteThread(thread);
}

static void timer_cases(void) {
    const SceUID timer = sceKernelCreateTimer("ro in", 0, NULL);
    const SceKernelSysClock time = 5000000;
    PRINT("set the timer's time from it",
        sceKernelSetTimerTime(timer, put(CLOCK_OFFSET, &time, sizeof(time))));
    const SceUInt64 now = sceKernelGetTimerTimeWide(timer);
    PRINT("set the timer's time from it: its time is that", now >= time && now < time + 1000000);

    const SceUInt64 interval = 1000000000;
    PRINT("set a timer event with the interval in it", sceKernelSetTimerEvent(timer,
        TIMER_TYPE_SET_EVENT, put(CLOCK_OFFSET, &interval, sizeof(interval)), 0));
    TimerInfo info = { .size = sizeof(info) };
    sceKernelGetTimerInfo(timer, &info);
    PRINT("set a timer event with the interval in it: its interval is that",
        info.interval == interval);
    sceKernelDeleteTimer(timer);
}

static void multiple_events_case(void) {
    const SceUID set = sceKernelCreateSimpleEvent("ro in set", 0, EVENT_BIT_A, NULL);
    const SceUID unset = sceKernelCreateSimpleEvent("ro in unset", 0, 0, NULL);
    const WaitEvent events[2] = { { unset, EVENT_BIT_A }, { set, EVENT_BIT_A } };
    ResultEvent results[2];
    sceClibMemset(results, 0, sizeof(results));
    SceUInt timeout = TEST_WAIT_US;
    PRINT("wait on several events listed in it", sceKernelWaitMultipleEvents(
        put(EVENTS_OFFSET, events, sizeof(events)), 2, EVENT_WAIT_MODE_OR, results, &timeout));
    PRINT("wait on several events listed in it: it reports the set one",
        results[0].eventId == set && results[0].resultPattern == EVENT_BIT_A);
    sceKernelDeleteSimpleEvent(set);
    sceKernelDeleteSimpleEvent(unset);
}

static void memory_block_cases(void) {
    const char *name = put(NAME_OFFSET, BLOCK_NAME, sizeof(BLOCK_NAME));
    const AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID block = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB,
        put(OPT_OFFSET, &opt, sizeof(opt)));
    PRINT("allocate a block with its name and option struct in it", test_created(block));
    if (block < 0)
        return;
    const SceUID opened = sceKernelOpenMemBlock(name, 0x10);
    PRINT("open it by the name in it", test_created(opened));
    if (opened >= 0)
        sceKernelCloseMemBlock(opened);
    sceKernelFreeMemBlock(block);
}

int main(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock("ro in", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("allocate the original", test_created(original));
    const SceUID opened = original >= 0 ? sceKernelOpenMemBlock("ro in", 0x10) : original;
    PRINT("open it read only", test_created(opened));
    if (opened < 0) {
        if (original >= 0)
            sceKernelFreeMemBlock(original);
        test_exit();
        return 0;
    }
    writable = base_of(original);
    readable = base_of(opened);

    name_cases();
    opt_cases();
    msg_pipe_cases();
    thread_args_case();
    timer_cases();
    multiple_events_case();
    memory_block_cases();

    sceKernelCloseMemBlock(opened);
    sceKernelFreeMemBlock(original);
    test_exit();
    return 0;
}
