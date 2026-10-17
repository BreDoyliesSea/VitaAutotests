// Thread and sync calls that write a value or a buffer for the app, given memory the app can only
// read for it, as readonly_out_info does for the info calls. The memory is a block opened with
// sceKernelOpenMemBlock with flags 0x10, which maps the memory of a 64 KiB USER_RW block with
// ENABLE_OPEN read only, so the test sets every word of it to a marker through the original and
// sees through it what a call wrote. Each case prints the result, whether the call changed any of
// the first 64 words, and what the call did to its object anyway, as values that don't change
// between runs: the pattern an event flag or a simple event has after a poll or a wait that
// clears it, the bytes left in a message pipe after a receive, and the count a semaphore has after
// a cancel. The cases, each on fresh objects:
// - an event flag poll and an event flag wait that end at once and clear, with the bits read only
// - a message pipe receive and try receive with the buffer read only, and a receive with the size
//   it writes read only, each with a message already in the pipe
// - sceKernelGetSystemTime, the timer's time and base, and the exit status of an ended thread.
//   sceKernelGetProcessTime is left out, since given memory it can't write it crashes the app on
//   a Vita instead of returning an error.
// - the cancel calls of a semaphore, an event flag and a message pipe, with their counts read only
// - a simple event's pattern, a poll of an auto reset event with its pattern or its user data read
//   only, and a wait on several events with the results read only
// The app itself never writes to the read only mapping, since that faults on a Vita.
//
// On a PS Vita TV on firmware 3.65 every call here fails with 0x80022005 (INVALID_MEMORY_ACCESS)
// and writes nothing into the read only mapping, but most of them still do their work first: the
// event flag poll and wait clear the bits, the three cancels set the semaphore's count, the flag's
// pattern and empty the pipe, the poll of an auto reset event takes its bits, and the receive with
// only its size read only takes the message. The two receives into a read only buffer are the
// exception: they take nothing, the message stays in the pipe and the size isn't written either.
// The exit status call works again into writable memory after.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define MARKER 0xA5A5A5A5
#define WORDS 64
#define OUT_MARKER 0xDEAD
#define STATUS 0x1234

// Writes the system time to time, as sceKernelGetSystemTimeWide returns it
int sceKernelGetSystemTime(SceKernelSysClock *time);

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static volatile SceUInt32 *writable;
static void *readable;

static int ends_with_status(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return STATUS;
}

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// Sets every word through the writable mapping to the marker
static void fill(void) {
    for (int i = 0; i < WORDS; i++)
        writable[i] = MARKER;
}

// Whether any word changed from what fill set
static int changed(void) {
    int any = 0;
    for (int i = 0; i < WORDS; i++)
        any |= writable[i] != MARKER;
    return any;
}

static SceUInt32 flag_pattern(SceUID flag) {
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(flag, &info);
    return info.currentPattern;
}

// The number of bytes in the pipe
static int pipe_bytes(SceUID pipe) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info.bufSize - info.freeSize;
}

static SceUInt32 event_pattern(SceUID event) {
    SceUInt32 pattern = 0;
    sceKernelGetEventPattern(event, &pattern);
    return pattern;
}

// Prints the result of a call into the read only memory and whether it changed the words
static void print_call(const char *name, int result) {
    PRINTF(result, "%s", name);
    PRINTF(changed(), "%s: the words changed", name);
}

static void event_flag_cases(void) {
    const unsigned int clear = SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR;
    SceUID flag = sceKernelCreateEventFlag("ro values", 0, 0x3, NULL);
    fill();
    print_call("poll a flag with clear, the bits into it",
        sceKernelPollEventFlag(flag, 0x1, clear, readable));
    PRINT("poll a flag with clear, the bits into it: the pattern after", flag_pattern(flag));
    sceKernelDeleteEventFlag(flag);

    flag = sceKernelCreateEventFlag("ro values", 0, 0x3, NULL);
    SceUInt timeout = TEST_WAIT_US;
    fill();
    print_call("wait on a flag with clear, the bits into it",
        sceKernelWaitEventFlag(flag, 0x1, clear, readable, &timeout));
    PRINT("wait on a flag with clear, the bits into it: the pattern after", flag_pattern(flag));
    sceKernelDeleteEventFlag(flag);
}

static void msg_pipe_cases(void) {
    static const char message[8] = "message";
    char got[8] = { 0 };
    SceSize size = OUT_MARKER;
    SceUInt timeout = TEST_WAIT_US;

    SceUID pipe = sceKernelCreateMsgPipe("ro values", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    sceKernelTrySendMsgPipe(pipe, (void *)message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    fill();
    print_call("receive into it", sceKernelReceiveMsgPipe(pipe, readable, sizeof(message),
        MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("receive into it: the size it wrote back", size);
    PRINT("receive into it: the bytes left in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("ro values", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    sceKernelTrySendMsgPipe(pipe, (void *)message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    size = OUT_MARKER;
    fill();
    print_call("try receive into it", sceKernelTryReceiveMsgPipe(pipe, readable,
        sizeof(message), MSG_PIPE_MODE_FULL, &size));
    PRINT("try receive into it: the size it wrote back", size);
    PRINT("try receive into it: the bytes left in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("ro values", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    sceKernelTrySendMsgPipe(pipe, (void *)message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    timeout = TEST_WAIT_US;
    fill();
    print_call("receive with the size into it", sceKernelReceiveMsgPipe(pipe, got,
        sizeof(got), MSG_PIPE_MODE_FULL, readable, &timeout));
    PRINT("receive with the size into it: it got the message",
        sceClibMemcmp(got, message, sizeof(message)) == 0);
    PRINT("receive with the size into it: the bytes left in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);
}

static void clock_cases(void) {
    fill();
    print_call("system time into it", sceKernelGetSystemTime(readable));

    const SceUID timer = sceKernelCreateTimer("ro values", 0, NULL);
    sceKernelStartTimer(timer);
    fill();
    print_call("timer time into it", sceKernelGetTimerTime(timer, readable));
    fill();
    print_call("timer base into it", sceKernelGetTimerBase(timer, readable));
    sceKernelDeleteTimer(timer);

    const SceUID thread = test_start_thread(ends_with_status);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    fill();
    print_call("exit status into it", sceKernelGetThreadExitStatus(thread, readable));
    int status = OUT_MARKER;
    PRINT("exit status into writable memory after", sceKernelGetThreadExitStatus(thread, &status));
    PRINT("exit status into writable memory after: the status", status);
    sceKernelDeleteThread(thread);
}

static void cancel_cases(void) {
    const SceUID sema = sceKernelCreateSema("ro values", 0, 0, 5, NULL);
    fill();
    print_call(
        "cancel a semaphore to 3, the count into it", sceKernelCancelSema(sema, 3, readable));
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(sema, &info);
    PRINT("cancel a semaphore to 3, the count into it: its count after", info.currentCount);
    sceKernelDeleteSema(sema);

    const SceUID flag = sceKernelCreateEventFlag("ro values", 0, 0, NULL);
    fill();
    print_call("cancel a flag to 0x5, the count into it",
        sceKernelCancelEventFlag(flag, 0x5, readable));
    PRINT("cancel a flag to 0x5, the count into it: the pattern after", flag_pattern(flag));
    sceKernelDeleteEventFlag(flag);

    const SceUID pipe = sceKernelCreateMsgPipe("ro values", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    char message[4] = { 1, 2, 3, 4 };
    sceKernelTrySendMsgPipe(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    fill();
    print_call("cancel a pipe, the counts into it",
        sceKernelCancelMsgPipe(pipe, readable, (int *)readable + 1));
    PRINT("cancel a pipe, the counts into it: the bytes left in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);
}

static void simple_event_cases(void) {
    SceUID event = sceKernelCreateSimpleEvent("ro values", 0, EVENT_BIT_A, NULL);
    fill();
    print_call("event pattern into it", sceKernelGetEventPattern(event, readable));
    sceKernelDeleteSimpleEvent(event);

    event = sceKernelCreateSimpleEvent("ro values", EVENT_ATTR_AUTO_RESET, EVENT_BIT_A, NULL);
    SceUInt64 user_data = OUT_MARKER;
    fill();
    print_call("poll an auto reset event, the pattern into it",
        sceKernelPollEvent(event, EVENT_BIT_A, readable, &user_data));
    PRINT("poll an auto reset event, the pattern into it: its pattern after", event_pattern(event));
    sceKernelDeleteSimpleEvent(event);

    event = sceKernelCreateSimpleEvent("ro values", EVENT_ATTR_AUTO_RESET, EVENT_BIT_A, NULL);
    SceUInt32 pattern = OUT_MARKER;
    fill();
    print_call("poll an auto reset event, the user data into it",
        sceKernelPollEvent(event, EVENT_BIT_A, &pattern, readable));
    PRINT("poll an auto reset event, the user data into it: the pattern it wrote", pattern);
    PRINT("poll an auto reset event, the user data into it: its pattern after",
        event_pattern(event));
    sceKernelDeleteSimpleEvent(event);

    const SceUID set = sceKernelCreateSimpleEvent("ro values set", 0, EVENT_BIT_A, NULL);
    const SceUID unset = sceKernelCreateSimpleEvent("ro values unset", 0, 0, NULL);
    WaitEvent events[2] = { { set, EVENT_BIT_A }, { unset, EVENT_BIT_A } };
    SceUInt timeout = TEST_WAIT_US;
    fill();
    print_call("wait on several events, the results into it",
        sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, readable, &timeout));
    sceKernelDeleteSimpleEvent(set);
    sceKernelDeleteSimpleEvent(unset);
}

int main(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock("ro values", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("allocate the original", test_created(original));
    const SceUID opened = original >= 0 ? sceKernelOpenMemBlock("ro values", 0x10) : original;
    PRINT("open it read only", test_created(opened));
    if (opened < 0) {
        if (original >= 0)
            sceKernelFreeMemBlock(original);
        test_exit();
        return 0;
    }
    writable = base_of(original);
    readable = (void *)base_of(opened);

    event_flag_cases();
    msg_pipe_cases();
    clock_cases();
    cancel_cases();
    simple_event_cases();

    sceKernelCloseMemBlock(opened);
    sceKernelFreeMemBlock(original);
    test_exit();
    return 0;
}
