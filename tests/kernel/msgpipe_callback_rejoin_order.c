// What a message pipe receive with callbacks gets when its thread comes back from a callback while
// the pipe holds bytes sent during the callback and a sender waits behind them. Every thread is
// pinned to core 0. The main thread runs at 0x60, the receiver T at 0xB0 and the sender S at 0xA0.
// T waits with sceKernelReceiveMsgPipeCB for a FULL receive of 4 bytes on an empty pipe of 4096.
// The main thread notifies T's callback, which blocks on a gate until the main thread signals it.
// Meanwhile the main thread sends 4092 older bytes, the letters a to z over and over, with a try
// send, which go into the pipe since no receive waits, and S waits with a FULL send of 8 newer
// bytes, "ABCDEFGH", which don't fit. Then the main thread signals the gate, so T's wait goes on
// after its callback. Every wait has a finite timeout. Once T's wait returned and S's send ended,
// or after a few seconds, the test prints what T got, what S did, and the pipe's free size and
// waiting senders.
//
// On a PS Vita TV on firmware 3.65 T gets the 4 older bytes "abcd" first, then S's 8 bytes fit and
// it is done, so the pipe ends full with no sender waiting.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096
#define OLDER 4092
#define MAIN_PRIORITY 0x60
#define T_PRIORITY 0xB0
#define S_PRIORITY 0xA0
#define LONG_TIMEOUT (3 * TEST_WAIT_US)
#define NOT_DONE 0xDEAD

static SceUID pipe, gate, callback;
static volatile int callback_ready, callback_started, t_done, s_done;
static volatile int t_result = NOT_DONE, s_result = NOT_DONE;
static SceSize t_size = NOT_DONE, s_size = NOT_DONE;
static char t_buf[5];
static char older[OLDER];

// Runs inside T's wait and blocks on the gate until the main thread opens it
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_started = 1;
    SceUInt timeout = LONG_TIMEOUT;
    sceKernelWaitSema(gate, 1, &timeout);
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    callback_ready = 1;
    SceUInt timeout = LONG_TIMEOUT;
    t_result = sceKernelReceiveMsgPipeCB(pipe, t_buf, 4, MSG_PIPE_MODE_FULL, &t_size, &timeout);
    t_done = 1;
    sceKernelDeleteCallback(callback);
    return 0;
}

static int s_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = LONG_TIMEOUT;
    s_result = sceKernelSendMsgPipe(pipe, "ABCDEFGH", 8, MSG_PIPE_MODE_FULL, &s_size, &timeout);
    s_done = 1;
    return 0;
}

static SceUID start(SceKernelThreadEntry entry, int priority) {
    const SceUID thread = sceKernelCreateThread("thread", entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    return thread;
}

static SceKernelMppInfo pipe_info(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info;
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    for (int i = 0; i < OLDER; i++)
        older[i] = 'a' + i % 26;
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);

    const SceUID t = start(t_entry, T_PRIORITY);
    TEST_WAIT_FOR(callback_ready && test_in_wait(t, WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB, pipe));
    const int t_waits = test_in_wait(t, WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB, pipe);
    const int notify = sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(callback_started);

    // No receive waits while T runs its callback, so the try send puts the bytes into the pipe
    SceSize older_size = NOT_DONE;
    const int older_send = sceKernelTrySendMsgPipe(pipe, older, OLDER, MSG_PIPE_MODE_FULL, &older_size);
    const SceUID s = start(s_entry, S_PRIORITY);
    TEST_WAIT_FOR(test_in_wait(s, WAIT_TYPE_MSG_PIPE, pipe));
    const int s_waits = test_in_wait(s, WAIT_TYPE_MSG_PIPE, pipe);
    const SceKernelMppInfo before = pipe_info();

    sceKernelSignalSema(gate, 1);
    TEST_WAIT_FOR(t_done);
    TEST_WAIT_FOR(s_done);
    const SceKernelMppInfo after = pipe_info();
    // Ends a wait that still goes on, if any, so the joins return
    sceKernelCancelMsgPipe(pipe, NULL, NULL);
    test_join_thread(t);
    test_join_thread(s);
    sceKernelDeleteMsgPipe(pipe);
    sceKernelDeleteSema(gate);

    PRINT("T waits with callbacks before the notify", t_waits);
    PRINT("the notify", notify);
    PRINT("the try send of the older bytes during the callback", older_send);
    PRINT("its size", older_size);
    PRINT("S waits before T is back", s_waits);
    PRINT("the free size before T is back", before.freeSize);
    PRINT("T's receive", t_result);
    PRINTF(t_size, "T got \"%s\", size", t_buf);
    PRINT("S's send", s_result);
    PRINT("S's size", s_size);
    PRINT("the free size after both ended", after.freeSize);
    PRINT("the waiting senders after both ended", after.numSendWaitThreads);
    test_exit();
    return 0;
}
