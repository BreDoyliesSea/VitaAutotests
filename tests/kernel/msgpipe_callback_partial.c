// What a message pipe receive does that got part of its bytes and then leaves the pipe's queue to
// run a callback. Receiver R waits with sceKernelReceiveMsgPipeCB for 8 bytes in FULL mode, with a
// timeout of 4 s, on an empty pipe of 4096 bytes that queues its receivers by priority. R creates its
// own callback. Every thread is pinned to core 0, the main thread runs at 0x60 and R at 0xB0. The
// main thread sends "ABCD" with a FULL send, which goes to R's buffer and leaves R waiting for 4
// more bytes, checks that R still waits, and notifies R's callback. Two cases:
// - the callback returns at once. Once it ran and R waits again, the main thread sends "EFGH".
// - the callback waits on a gate semaphore. While R is in that wait, which is outside the queue, the
//   main thread reads the pipe, sends "EFGH" and reads the pipe again, then signals the gate. The
//   send finds no receiver, so its bytes can only go into the pipe's buffer.
// Each case prints the sends' results and sizes, whether R waits after the first send, the notify's
// result, the receivers the pipe counts and the bytes in the pipe at the points above, R's receive
// result and size, whether its buffer holds "ABCDEFGH", the runs of its callback and the bytes left
// in the pipe once R ended. Every wait has a finite timeout.
//
// On a PS Vita TV on firmware 3.65 R still waits after the first send, since it holds 4 of its 8
// bytes. Once R left the queue, the pipe counts no receiver. In the first case R is back in the queue
// when "EFGH" is sent and the send completes it. In the second the pipe's buffer holds the 4 bytes
// during the gate, and R takes them when it comes back from its callback. Both cases end with R's
// receive returning 0 with all 8 bytes, "ABCDEFGH", and the pipe empty.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096
#define MAIN_PRIORITY 0x60
#define R_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
#define R_TIMEOUT_US 4000000
#define GATE_TIMEOUT_US 6000000

static SceUID pipe, gate, callback;
static int gated;
static volatile int callback_ready, callback_runs, r_result = NOT_DONE;
static SceSize r_size = NOT_DONE;
static char r_buffer[9];

// Returns at once, or waits on the gate until the main thread signals it
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    if (gated) {
        SceUInt timeout = GATE_TIMEOUT_US;
        sceKernelWaitSema(gate, 1, &timeout);
    }
    callback_runs++;
    return 0;
}

// Creates its callback and receives 8 bytes in FULL mode with callbacks
static int r_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    callback_ready = 1;
    SceUInt timeout = R_TIMEOUT_US;
    r_result = sceKernelReceiveMsgPipeCB(pipe, r_buffer, 8, MSG_PIPE_MODE_FULL, &r_size, &timeout);
    sceKernelDeleteCallback(callback);
    return 0;
}

static SceKernelMppInfo pipe_info(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info;
}

// Sends 4 bytes in FULL mode and returns the result, the size goes to *size
static int send4(const char *bytes, SceSize *size) {
    SceUInt timeout = TEST_WAIT_US;
    *size = NOT_DONE;
    return sceKernelSendMsgPipe(pipe, (void *)bytes, 4, MSG_PIPE_MODE_FULL, size, &timeout);
}

static void run_case(const char *name, int gate_the_callback) {
    gated = gate_the_callback;
    callback_ready = callback_runs = 0;
    r_result = NOT_DONE;
    r_size = NOT_DONE;
    sceClibMemset(r_buffer, 0, sizeof(r_buffer));
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, MSG_PIPE_ATTR_RECEIVE_PRIO, CAPACITY, NULL);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);

    const SceUInt32 wait_type = WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB;
    const SceUID r = sceKernelCreateThread("r", r_entry, R_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(r, 0, NULL);
    TEST_WAIT_FOR(callback_ready && test_in_wait(r, wait_type, pipe));
    const int waits_before = test_in_wait(r, wait_type, pipe);

    SceSize first_size, second_size;
    const int first_send = send4("ABCD", &first_size);
    const int waits_after_send = test_in_wait(r, wait_type, pipe);
    const int notify = sceKernelNotifyCallback(callback, 1);

    int waits_again_or_in_gate, receivers, used_before, used_after = NOT_DONE;
    if (gated) {
        TEST_WAIT_FOR(test_in_wait(r, WAIT_TYPE_SEMA, gate));
        waits_again_or_in_gate = test_in_wait(r, WAIT_TYPE_SEMA, gate);
    } else {
        TEST_WAIT_FOR(callback_runs > 0);
        TEST_WAIT_FOR(test_in_wait(r, wait_type, pipe));
        waits_again_or_in_gate = test_in_wait(r, wait_type, pipe);
    }
    // R is blocked, so the pipe's state is stable
    SceKernelMppInfo info = pipe_info();
    receivers = info.numReceiveWaitThreads;
    used_before = CAPACITY - info.freeSize;

    const int second_send = send4("EFGH", &second_size);
    if (gated) {
        info = pipe_info();
        used_after = CAPACITY - info.freeSize;
        sceKernelSignalSema(gate, 1);
    }
    test_join_thread(r);
    info = pipe_info();
    const int left = CAPACITY - info.freeSize;
    // Ends a wait that still goes on, if any, so nothing is left behind
    sceKernelCancelMsgPipe(pipe, NULL, NULL);
    sceKernelDeleteMsgPipe(pipe);
    sceKernelDeleteSema(gate);

    PRINTF(waits_before, "%s: R waits with callbacks before the first send", name);
    PRINTF(first_send, "%s: the first send", name);
    PRINTF(first_size, "%s: the first send's size", name);
    PRINTF(waits_after_send, "%s: R still waits after the first send", name);
    PRINTF(notify, "%s: the notify", name);
    if (gated)
        PRINTF(waits_again_or_in_gate, "%s: R is in the gate's wait", name);
    else
        PRINTF(waits_again_or_in_gate, "%s: R waits again after its callback", name);
    PRINTF(receivers, "%s: the receivers the pipe counts then", name);
    PRINTF(used_before, "%s: the bytes in the pipe then", name);
    PRINTF(second_send, "%s: the second send", name);
    PRINTF(second_size, "%s: the second send's size", name);
    if (gated)
        PRINTF(used_after, "%s: the bytes in the pipe after the second send", name);
    PRINTF(r_result, "%s: R's receive", name);
    PRINTF(r_size, "%s: R's size", name);
    PRINTF(sceClibMemcmp(r_buffer, "ABCDEFGH", 8) == 0, "%s: R's buffer reads ABCDEFGH", name);
    PRINTF(callback_runs, "%s: runs of R's callback", name);
    PRINTF(left, "%s: the bytes left in the pipe", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("callback returns", 0);
    run_case("callback waits on a gate", 1);

    test_exit();
    return 0;
}
