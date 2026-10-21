// A thread A receives from a message pipe with callbacks, its callback is notified, and the callback
// receives from the same pipe. The pipe holds 2 bytes, A receives 4 bytes in FULL mode, and the
// callback receives 2 bytes in ASAP mode with a timeout of 500 ms, so its wait ends either way. The
// callback records the pipe's receivers, senders and bytes before and after its receive, and the
// receive's result and size. Then the main thread cancels the pipe, which ends A's receive, and the
// test prints A's result, its size and whether its buffer holds the bytes sent. A's own wait is long,
// so the test always ends. The test only uses blocking calls and no sender, since a try receive
// that leaves a sender waiting can fail to return. The main thread sends before anything waits.
//
// On a PS Vita TV on firmware 3.65 A's receive takes the 2 bytes in the pipe when it starts to
// wait, so the callback finds the pipe empty with no receiver counted, and its receive times out. A
// waits again afterwards, and the cancel ends its wait with the 2 bytes, which it keeps.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
// The timeout of the callback's receive
#define INNER_TIMEOUT_US 500000
// The timeout of A's receive, which the main thread ends
#define OUTER_TIMEOUT_US (3 * TEST_WAIT_US)
#define PIPE_SIZE 4096

static SceUID pipe, callback;
static volatile int callback_done, a_result, inner_result;
static volatile SceSize a_moved, inner_moved;
static volatile int before[3], after[3];
static char a_buffer[4];

// Records the pipe's state before and after a receive from the same pipe, which times out at 500 ms
static int inner_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = INNER_TIMEOUT_US;
    SceKernelMppInfo info = { .size = sizeof(info) };
    char buffer[2];
    SceSize moved = 0;
    sceKernelGetMsgPipeInfo(pipe, &info);
    before[0] = info.numReceiveWaitThreads;
    before[1] = info.numSendWaitThreads;
    before[2] = info.bufSize - info.freeSize;
    inner_result = sceKernelReceiveMsgPipe(pipe, buffer, sizeof(buffer), MSG_PIPE_MODE_ASAP, &moved, &timeout);
    inner_moved = moved;
    sceKernelGetMsgPipeInfo(pipe, &info);
    after[0] = info.numReceiveWaitThreads;
    after[1] = info.numSendWaitThreads;
    after[2] = info.bufSize - info.freeSize;
    callback_done = 1;
    return 0;
}

// Receives 4 bytes in FULL mode with callbacks, for more than the pipe holds
static int a_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("inner", 0, inner_callback, NULL);
    SceUInt timeout = OUTER_TIMEOUT_US;
    SceSize moved = 0;
    a_result = sceKernelReceiveMsgPipeCB(pipe, a_buffer, sizeof(a_buffer), MSG_PIPE_MODE_FULL, &moved, &timeout);
    a_moved = moved;
    sceKernelDeleteCallback(callback);
    return 0;
}

int main(void) {
    const char *name = "message pipe";
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, PIPE_SIZE, NULL);
    SceUInt send_timeout = TEST_WAIT_US;
    SceSize sent = NOT_DONE;
    PRINTF(sceKernelSendMsgPipe(pipe, "ab", 2, MSG_PIPE_MODE_FULL, &sent, &send_timeout), "%s: send 2 bytes", name);
    PRINTF(sent, "%s: the bytes sent", name);

    a_result = inner_result = NOT_DONE;
    a_moved = inner_moved = NOT_DONE;
    for (int i = 0; i < 3; i++)
        before[i] = after[i] = NOT_DONE;
    const SceUID a = test_start_thread(a_thread);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB, pipe));
    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(callback_done);
    PRINTF(before[0], "%s: in the callback, the receivers before its wait", name);
    PRINTF(before[1], "%s: in the callback, the senders before its wait", name);
    PRINTF(before[2], "%s: in the callback, the bytes in the pipe before its wait", name);
    PRINTF(inner_result, "%s: the callback's receive", name);
    PRINTF(inner_moved, "%s: the callback's receive size", name);
    PRINTF(after[0], "%s: in the callback, the receivers after its wait", name);
    PRINTF(after[1], "%s: in the callback, the senders after its wait", name);
    PRINTF(after[2], "%s: in the callback, the bytes in the pipe after its wait", name);

    // Waits until A receives again after its callback, or has ended
    TEST_WAIT_FOR(a_result != NOT_DONE || test_in_wait(a, WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB, pipe));
    PRINTF(test_in_wait(a, WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB, pipe), "%s: A waits again after the callback", name);
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINTF(info.numReceiveWaitThreads, "%s: the receivers then", name);
    PRINTF(info.numSendWaitThreads, "%s: the senders then", name);
    PRINTF(info.bufSize - info.freeSize, "%s: the bytes in the pipe then", name);
    int senders = NOT_DONE;
    int receivers = NOT_DONE;
    PRINTF(sceKernelCancelMsgPipe(pipe, &senders, &receivers), "%s: cancel the pipe", name);
    PRINTF(senders, "%s: the senders it ended", name);
    PRINTF(receivers, "%s: the receivers it ended", name);
    test_join_thread(a);
    PRINTF(a_result, "%s: A's receive", name);
    PRINTF(a_moved, "%s: A's receive size", name);
    PRINTF(sceClibMemcmp(a_buffer, "ab", 2) == 0, "%s: A's buffer starts with the bytes sent", name);
    sceKernelDeleteMsgPipe(pipe);

    test_exit();
    return 0;
}
