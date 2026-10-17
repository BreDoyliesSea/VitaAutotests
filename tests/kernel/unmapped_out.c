// Thread, sync and memory block calls that write a result for the app, given an address that no
// memory block maps for it. The address is the base of a 64 KiB USER_RW block freed right before
// the call, which is then in no block, as the test checks first with sceKernelFindMemBlockByAddr.
// Nothing that could make a block runs between the free and the call, so objects that need a
// thread are made before. Each case prints the result, and for a call that changes its object
// whatever it can write, what it did: the pattern an event flag or a simple event has after a
// poll or a wait that clears it, the bytes left in a message pipe after a receive, and the count
// a semaphore has after a cancel. The cases follow readonly_out_values: the event flag poll and
// wait, the message pipe receives, the clocks but sceKernelGetProcessTime, which crashes the app
// on a Vita given memory it can't write, a thread's exit status, the cancel calls, a simple
// event's pattern and poll, and a wait on several events. Then sceKernelGetMemBlockBase,
// sceKernelGetMemBlockInfoByAddr and sceKernelGetFreeMemorySize. The info calls of
// readonly_out_info are left out, since the first of them, sceKernelGetThreadInfo, crashes the
// app on a Vita when the struct is in no block. The app itself never touches the address.
//
// On a PS Vita TV on firmware 3.65 the freed block's base is in no block (0x80022007, VA2PA_FAULT),
// and every call given it fails with 0x80022005 (INVALID_MEMORY_ACCESS), except
// sceKernelGetFreeMemorySize, which gives 0x80020005 (INVALID_ARGUMENT). The side effects match
// readonly_out_values: the flag's bits are cleared, the cancels happen, the auto reset event's bits
// are taken and the receive with its size there takes the message, while the receives into a buffer
// there take nothing and write no size.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define STATUS 0x1234

// Writes the system time to time, as sceKernelGetSystemTimeWide returns it
int sceKernelGetSystemTime(SceKernelSysClock *time);

static int ends_with_status(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return STATUS;
}

// Allocates a block and frees it, and returns its base, which is in no block until the next
// allocation
static void *unmapped(void) {
    const SceUID block
        = sceKernelAllocMemBlock("unmapped out", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    sceKernelFreeMemBlock(block);
    return base;
}

static SceUInt32 flag_pattern(SceUID flag) {
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(flag, &info);
    return info.currentPattern;
}

static int pipe_bytes(SceUID pipe) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info.bufSize - info.freeSize;
}

static SceUID pipe_with_message(void) {
    static const char message[8] = "message";
    const SceUID pipe = sceKernelCreateMsgPipe("unmapped out", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    sceKernelTrySendMsgPipe(pipe, (void *)message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    return pipe;
}

static void event_flag_cases(void) {
    const unsigned int clear = SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR;
    SceUID flag = sceKernelCreateEventFlag("unmapped out", 0, 0x3, NULL);
    PRINT("poll a flag with clear, the bits there",
        sceKernelPollEventFlag(flag, 0x1, clear, unmapped()));
    PRINT("poll a flag with clear, the bits there: the pattern after", flag_pattern(flag));
    sceKernelDeleteEventFlag(flag);

    flag = sceKernelCreateEventFlag("unmapped out", 0, 0x3, NULL);
    SceUInt timeout = TEST_WAIT_US;
    PRINT("wait on a flag with clear, the bits there",
        sceKernelWaitEventFlag(flag, 0x1, clear, unmapped(), &timeout));
    PRINT("wait on a flag with clear, the bits there: the pattern after", flag_pattern(flag));
    sceKernelDeleteEventFlag(flag);
}

static void msg_pipe_cases(void) {
    char got[8];
    SceSize size = 0xDEAD;
    SceUInt timeout = TEST_WAIT_US;

    SceUID pipe = pipe_with_message();
    PRINT("receive there", sceKernelReceiveMsgPipe(pipe, unmapped(), sizeof(got),
        MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("receive there: the size it wrote back", size);
    PRINT("receive there: the bytes left in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);

    pipe = pipe_with_message();
    size = 0xDEAD;
    PRINT("try receive there",
        sceKernelTryReceiveMsgPipe(pipe, unmapped(), sizeof(got), MSG_PIPE_MODE_FULL, &size));
    PRINT("try receive there: the size it wrote back", size);
    PRINT("try receive there: the bytes left in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);

    pipe = pipe_with_message();
    timeout = TEST_WAIT_US;
    PRINT("receive with the size there", sceKernelReceiveMsgPipe(pipe, got, sizeof(got),
        MSG_PIPE_MODE_FULL, unmapped(), &timeout));
    PRINT("receive with the size there: the bytes left in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);
}

static void clock_cases(void) {
    PRINT("system time there", sceKernelGetSystemTime(unmapped()));
    const SceUID timer = sceKernelCreateTimer("unmapped out", 0, NULL);
    sceKernelStartTimer(timer);
    PRINT("timer time there", sceKernelGetTimerTime(timer, unmapped()));
    PRINT("timer base there", sceKernelGetTimerBase(timer, unmapped()));
    sceKernelDeleteTimer(timer);

    const SceUID thread = test_start_thread(ends_with_status);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    PRINT("exit status there", sceKernelGetThreadExitStatus(thread, unmapped()));
    sceKernelDeleteThread(thread);
}

static void cancel_cases(void) {
    const SceUID sema = sceKernelCreateSema("unmapped out", 0, 0, 5, NULL);
    PRINT("cancel a semaphore to 3, the count there", sceKernelCancelSema(sema, 3, unmapped()));
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(sema, &info);
    PRINT("cancel a semaphore to 3, the count there: its count after", info.currentCount);
    sceKernelDeleteSema(sema);

    const SceUID flag = sceKernelCreateEventFlag("unmapped out", 0, 0, NULL);
    PRINT("cancel a flag to 0x5, the count there", sceKernelCancelEventFlag(flag, 0x5, unmapped()));
    PRINT("cancel a flag to 0x5, the count there: the pattern after", flag_pattern(flag));
    sceKernelDeleteEventFlag(flag);

    const SceUID pipe = pipe_with_message();
    int *const counts = unmapped();
    PRINT("cancel a pipe, the counts there", sceKernelCancelMsgPipe(pipe, counts, counts + 1));
    PRINT("cancel a pipe, the counts there: the bytes left in the pipe", pipe_bytes(pipe));
    sceKernelDeleteMsgPipe(pipe);
}

static void simple_event_cases(void) {
    SceUID event = sceKernelCreateSimpleEvent("unmapped out", 0, EVENT_BIT_A, NULL);
    PRINT("event pattern there", sceKernelGetEventPattern(event, unmapped()));
    sceKernelDeleteSimpleEvent(event);

    event = sceKernelCreateSimpleEvent("unmapped out", EVENT_ATTR_AUTO_RESET, EVENT_BIT_A, NULL);
    SceUInt64 user_data = 0;
    PRINT("poll an auto reset event, the pattern there",
        sceKernelPollEvent(event, EVENT_BIT_A, unmapped(), &user_data));
    SceUInt32 pattern = 0;
    sceKernelGetEventPattern(event, &pattern);
    PRINT("poll an auto reset event, the pattern there: its pattern after", pattern);
    sceKernelDeleteSimpleEvent(event);

    const SceUID set = sceKernelCreateSimpleEvent("unmapped out set", 0, EVENT_BIT_A, NULL);
    const SceUID unset = sceKernelCreateSimpleEvent("unmapped out unset", 0, 0, NULL);
    WaitEvent events[2] = { { set, EVENT_BIT_A }, { unset, EVENT_BIT_A } };
    SceUInt timeout = TEST_WAIT_US;
    PRINT("wait on several events, the results there",
        sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, unmapped(), &timeout));
    sceKernelDeleteSimpleEvent(set);
    sceKernelDeleteSimpleEvent(unset);
}

static void memory_block_cases(void) {
    const SceUID block
        = sceKernelAllocMemBlock("unmapped out", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    PRINT("block base there", sceKernelGetMemBlockBase(block, unmapped()));
    PRINT("block info by address there", sceKernelGetMemBlockInfoByAddr(base, unmapped()));
    PRINT("free memory size there", sceKernelGetFreeMemorySize(unmapped()));
    sceKernelFreeMemBlock(block);
}

// The info calls, each on an object made before the address
int main(void) {
    PRINT("find the base of a freed block", sceKernelFindMemBlockByAddr(unmapped(), 0));

    event_flag_cases();
    msg_pipe_cases();
    clock_cases();
    cancel_cases();
    simple_event_cases();
    memory_block_cases();

    test_exit();
    return 0;
}
