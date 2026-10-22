// Message pipe sends and receives of 0 bytes whose call could wait. Each call has a timeout of 1
// second in a variable, and the size is set to 0xDEAD first. For each call the test prints the
// result, the size, whether the timeout variable changed (a wait that ends writes it), whether the
// call returned within 200 ms, and the pipe's free size afterwards. Each case uses a new pipe of
// 4096 bytes. The cases:
// - sceKernelSendMsgPipe of 0 bytes in ASAP and FULL mode, on an empty pipe and on a full one
// - sceKernelReceiveMsgPipe of 0 bytes in ASAP and FULL mode, on an empty pipe and on one that
//   holds 16 bytes
// - sceKernelSendMsgPipeCB and sceKernelReceiveMsgPipeCB of 0 bytes in ASAP mode, on the same pipes
// - a full pipe with a FULL sender of 8 bytes waiting in its own thread, and a receive of 0 bytes
//   in ASAP mode on it. After the receive the test prints whether the sender still waits, the
//   waiting senders and the free size. Then it cancels the pipe, joins the sender and prints its
//   result and size
// Only blocking calls run here, and every wait has a finite timeout. No try receive runs while a
// sender waits, since that can freeze a Vita for minutes.
//
// On a PS Vita TV on firmware 3.65 every one of these calls returns 0 at once with a size of 0 and
// leaves the timeout alone, in ASAP and in FULL mode, and the receive of 0 bytes leaves the waiting
// sender waiting.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// A Vita rounds a pipe up to 4 KiB, so the pipe and the buffer that fills it are that size
#define CAPACITY 4096
#define HELD 16
#define NOT_WRITTEN 0xDEAD
#define TIMEOUT_US 1000000
// A call that returns at once takes far less than this, and one that waits for its timeout far more
#define QUICK_US 200000

enum Call { SEND, SEND_CB, RECEIVE, RECEIVE_CB };
enum Fill { EMPTY_PIPE, FULL_PIPE, SOME_BYTES };

typedef struct Case {
    const char *name;
    int call;
    SceUInt32 mode;
    int fill;
} Case;

static const Case cases[] = {
    { "send of 0 bytes ASAP on an empty pipe", SEND, MSG_PIPE_MODE_ASAP, EMPTY_PIPE },
    { "send of 0 bytes ASAP on a full pipe", SEND, MSG_PIPE_MODE_ASAP, FULL_PIPE },
    { "send of 0 bytes FULL on an empty pipe", SEND, MSG_PIPE_MODE_FULL, EMPTY_PIPE },
    { "send of 0 bytes FULL on a full pipe", SEND, MSG_PIPE_MODE_FULL, FULL_PIPE },
    { "receive of 0 bytes ASAP on an empty pipe", RECEIVE, MSG_PIPE_MODE_ASAP, EMPTY_PIPE },
    { "receive of 0 bytes ASAP, pipe with bytes", RECEIVE, MSG_PIPE_MODE_ASAP, SOME_BYTES },
    { "receive of 0 bytes FULL on an empty pipe", RECEIVE, MSG_PIPE_MODE_FULL, EMPTY_PIPE },
    { "receive of 0 bytes FULL, pipe with bytes", RECEIVE, MSG_PIPE_MODE_FULL, SOME_BYTES },
    { "send CB of 0 bytes ASAP on an empty pipe", SEND_CB, MSG_PIPE_MODE_ASAP, EMPTY_PIPE },
    { "send CB of 0 bytes ASAP on a full pipe", SEND_CB, MSG_PIPE_MODE_ASAP, FULL_PIPE },
    { "receive CB of 0 bytes ASAP on an empty pipe", RECEIVE_CB, MSG_PIPE_MODE_ASAP, EMPTY_PIPE },
    { "receive CB of 0 bytes ASAP, pipe with bytes", RECEIVE_CB, MSG_PIPE_MODE_ASAP, SOME_BYTES },
};

static SceUID pipe;
static SceUID sender;
static char buf[CAPACITY];
static volatile int sender_result, sender_done;
static SceSize sender_size;

static SceKernelMppInfo pipe_info(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info;
}

// Creates a new pipe and puts bytes into it with a try send, which has room for all of them. No
// thread waits on the pipe yet, so the try call is safe.
static void new_pipe(int fill) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    SceSize moved;
    if (fill == FULL_PIPE)
        sceKernelTrySendMsgPipe(pipe, buf, CAPACITY, MSG_PIPE_MODE_FULL, &moved);
    else if (fill == SOME_BYTES)
        sceKernelTrySendMsgPipe(pipe, buf, HELD, MSG_PIPE_MODE_FULL, &moved);
}

// Moves 0 bytes with the call of the case
static int transfer(int call, SceUInt32 mode, SceSize *size, SceUInt32 *timeout) {
    switch (call) {
    case SEND: return sceKernelSendMsgPipe(pipe, buf, 0, mode, size, timeout);
    case SEND_CB: return sceKernelSendMsgPipeCB(pipe, buf, 0, mode, size, timeout);
    case RECEIVE: return sceKernelReceiveMsgPipe(pipe, buf, 0, mode, size, timeout);
    default: return sceKernelReceiveMsgPipeCB(pipe, buf, 0, mode, size, timeout);
    }
}

// Runs one transfer of 0 bytes and prints what it did
static void run_transfer(const char *name, int call, SceUInt32 mode) {
    SceSize size = NOT_WRITTEN;
    SceUInt32 timeout = TIMEOUT_US;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    const int result = transfer(call, mode, &size, &timeout);
    const SceUInt64 elapsed = sceKernelGetProcessTimeWide() - start;
    const SceKernelMppInfo info = pipe_info();

    PRINTF(result, "%s: the call", name);
    PRINTF(size, "%s: its size", name);
    PRINTF(timeout != TIMEOUT_US, "%s: the timeout changed", name);
    PRINTF(elapsed < QUICK_US, "%s: it returned within 200 ms", name);
    PRINTF(info.freeSize, "%s: the free size after it", name);
}

static int sender_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    // Far past the receive that may wait for its timeout of 1 second, the cancel ends this wait
    SceUInt32 timeout = 3 * TEST_WAIT_US;
    sender_result = sceKernelSendMsgPipe(pipe, buf, 8, MSG_PIPE_MODE_FULL, &sender_size, &timeout);
    sender_done = 1;
    return 0;
}

static int sender_waits(void) {
    return !sender_done && test_in_wait(sender, WAIT_TYPE_MSG_PIPE, pipe);
}

// Waits until the sender is done or waits on the pipe on 20 polls in a row, at most 5 s. A thread
// that was just woken may show its wait until it runs, so one look isn't enough.
static void settle(void) {
    int stable = 0;
    for (int poll = 0; poll < 5000 && stable < 20; poll++) {
        stable = (sender_done || sender_waits()) ? stable + 1 : 0;
        if (stable < 20)
            sceKernelDelayThread(1000);
    }
}

// A receive of 0 bytes in ASAP mode on a full pipe while a FULL sender waits on it
static void run_sender_case(void) {
    const char *name = "receive of 0 bytes ASAP on a full pipe with a FULL sender";
    new_pipe(FULL_PIPE);
    sender_result = NOT_WRITTEN;
    sender_done = 0;
    sender_size = NOT_WRITTEN;
    sender = test_start_thread(sender_entry);
    TEST_WAIT_FOR(pipe_info().numSendWaitThreads == 1);
    test_wait_until_waiting(sender);
    const SceKernelMppInfo before = pipe_info();
    PRINTF(before.numSendWaitThreads, "%s: the waiting senders before the receive", name);

    run_transfer(name, RECEIVE, MSG_PIPE_MODE_ASAP);

    // Every other thread of the test is blocked or done after this, so the values can be read
    settle();
    const int waits = sender_waits();
    const SceKernelMppInfo after = pipe_info();
    int canceled_senders = -1, canceled_receivers = -1;
    const int canceled = sceKernelCancelMsgPipe(pipe, &canceled_senders, &canceled_receivers);
    test_join_thread(sender);

    PRINTF(waits, "%s: the sender still waits", name);
    PRINTF(after.numSendWaitThreads, "%s: the waiting senders after the receive", name);
    PRINTF(after.freeSize, "%s: the free size after the receive", name);
    PRINTF(canceled, "%s: cancel the pipe", name);
    PRINTF(canceled_senders, "%s: the senders the cancel ended", name);
    PRINTF(canceled_receivers, "%s: the receivers the cancel ended", name);
    PRINTF(sender_result, "%s: the sender's result", name);
    PRINTF(sender_size, "%s: the sender's size", name);
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        new_pipe(cases[i].fill);
        run_transfer(cases[i].name, cases[i].call, cases[i].mode);
        sceKernelDeleteMsgPipe(pipe);
    }
    run_sender_case();

    test_exit();
    return 0;
}
