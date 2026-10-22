// Where a message pipe receiver goes in the queue when it joins it again, after it ran a callback
// or after a priority change, while the receiver at the head of the queue holds part of its bytes.
// The pipe has attr 0x2, which queues receivers by priority. Receivers A, B and C each wait for 8
// bytes FULL on core 0, while this thread runs on core 1. The bytes are the letters "a" to "x" in
// order, so what each receiver got shows the order the bytes were handed out in. Four cases, each
// on a new pipe:
// - Rejoin after a callback, with a partial head: A, of the lowest priority, waits first and gets
//   "abcd" from a try send, so it holds half of its bytes. Then B waits with
//   sceKernelReceiveMsgPipeCB, and C, of a higher priority than B, waits after it. Both are of
//   higher priority than A. This thread notifies B's callback, and once B ran it and waits again,
//   one try send of the other 20 bytes goes to the receivers in queue order.
// - The same without the first send, so A holds nothing and the 24 bytes go in one send.
// - Priority change, with a partial head: A, of the highest priority, gets "abcd" the same way,
//   then B and C wait behind it, C of the lowest priority. Then C is raised above A and B, and one
//   try send gives the other 20 bytes.
// - The same without the first send.
// C is of a higher priority than B in the rejoin cases, so B going back behind C, to the end of the
// queue, or in front of A each give another order than B going right behind A.
//
// On a PS Vita TV on firmware 3.65 a receiver whose place by priority is the front of a queue whose
// head holds part of its bytes goes right behind that head instead, also when it joins again after
// a callback or a priority change. In the first case B waits right behind A, and C, ahead of B by
// priority, goes right behind A too. After B's callback B goes right behind A again, ahead of C. So
// A gets "abcdefgh", B "ijklmnop" and C "qrstuvwx". With A holding nothing the receivers stay in
// priority order, so C gets "abcdefgh", B "ijklmnop" and A "qrstuvwx". With the priority change C
// goes right behind A, so A gets "abcdefgh", C "ijklmnop" and B "qrstuvwx". Without the first
// send C goes to the front, so C gets "abcdefgh", A "ijklmnop" and B "qrstuvwx".
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096
#define RECEIVERS 3
#define RECEIVE_SIZE 8
#define HEAD_BYTES 4
#define NO_CALLBACK -1
#define LONG_TIMEOUT (3 * TEST_WAIT_US)
#define NOT_DONE 0xDEAD

// The priority C is raised to, above every receiver's
#define RAISED_PRIORITY 0x70

enum Action { REJOIN, CHANGE };

static const char *const names[RECEIVERS] = { "A", "B", "C" };

// Eight bytes for each receiver
static const char stream[] = "abcdefghijklmnopqrstuvwx";
#define STREAM_SIZE (sizeof(stream) - 1)

static SceUID pipe, callback;
static SceUID threads[RECEIVERS];
// The receiver that waits with callbacks, or NO_CALLBACK
static volatile int callback_receiver;
static volatile int callback_ran;
static volatile int result[RECEIVERS];
static SceSize moved[RECEIVERS];
static char received[RECEIVERS][16];

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_ran = 1;
    return 0;
}

static int receiver(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = LONG_TIMEOUT;
    if (i == callback_receiver) {
        callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
        result[i] = sceKernelReceiveMsgPipeCB(pipe, received[i], RECEIVE_SIZE, MSG_PIPE_MODE_FULL, &moved[i], &timeout);
        sceKernelDeleteCallback(callback);
    } else {
        result[i] = sceKernelReceiveMsgPipe(pipe, received[i], RECEIVE_SIZE, MSG_PIPE_MODE_FULL, &moved[i], &timeout);
    }
    return 0;
}

// Starts receiver i on core 0 and waits until it is in its wait
static void start(int i, int priority) {
    result[i] = NOT_DONE;
    moved[i] = 0;
    sceClibMemset(received[i], 0, sizeof(received[i]));
    threads[i] = sceKernelCreateThread("receiver", receiver, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(threads[i], sizeof(i), &i);
    const SceUInt32 type = i == callback_receiver ? WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB : WAIT_TYPE_MSG_PIPE;
    TEST_WAIT_FOR(test_in_wait(threads[i], type, pipe));
}

static SceKernelMppInfo pipe_info(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info;
}

// Runs one case. The receivers wait in the order A, B, C with the given priorities.
static void run(const char *name, enum Action action, int partial, const int priorities[RECEIVERS]) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, MSG_PIPE_ATTR_RECEIVE_PRIO, CAPACITY, NULL);
    callback_receiver = action == REJOIN ? 1 : NO_CALLBACK;
    callback_ran = 0;
    callback = 0;
    int first_send = NOT_DONE, a_waits = 0, notify = NOT_DONE, change = NOT_DONE, b_waits_again = 0;
    SceSize first_size = NOT_DONE;
    size_t offset = 0;

    start(0, priorities[0]);
    if (partial) {
        first_send = sceKernelTrySendMsgPipe(pipe, (void *)stream, HEAD_BYTES, MSG_PIPE_MODE_FULL, &first_size);
        offset = HEAD_BYTES;
        // A waits for the other 4 bytes and no other thread runs, so this reads right after the send
        a_waits = test_in_wait(threads[0], WAIT_TYPE_MSG_PIPE, pipe);
    }
    start(1, priorities[1]);
    start(2, priorities[2]);
    if (action == REJOIN) {
        notify = sceKernelNotifyCallback(callback, 1);
        // The callback ran and B queued again, which brings the waiting receivers back to all three
        const SceUInt32 type = WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB;
        TEST_WAIT_FOR(callback_ran && pipe_info().numReceiveWaitThreads == RECEIVERS && test_in_wait(threads[1], type, pipe));
        b_waits_again = callback_ran && test_in_wait(threads[1], type, pipe);
    } else {
        change = sceKernelChangeThreadPriority(threads[2], RAISED_PRIORITY);
    }
    const int waiting = pipe_info().numReceiveWaitThreads;

    // Every receiver waits, so the bytes go to them in queue order and none stay in the pipe
    SceSize last_size = NOT_DONE;
    const int last_send = sceKernelTrySendMsgPipe(pipe, (void *)(stream + offset), STREAM_SIZE - offset, MSG_PIPE_MODE_FULL, &last_size);
    for (int i = 0; i < RECEIVERS; i++)
        test_join_thread(threads[i]);
    const SceKernelMppInfo info = pipe_info();
    sceKernelDeleteMsgPipe(pipe);

    if (partial) {
        PRINTF(first_send, "%s: the first send", name);
        PRINTF(first_size, "%s: the first send's size", name);
        PRINTF(a_waits, "%s: A still waits after the first send", name);
    }
    if (action == REJOIN) {
        PRINTF(notify, "%s: the notify of B's callback", name);
        PRINTF(b_waits_again, "%s: B ran its callback and waits again", name);
    } else {
        PRINTF(change, "%s: raising C's priority", name);
    }
    PRINTF(waiting, "%s: the receivers waiting before the last send", name);
    PRINTF(last_send, "%s: the last send", name);
    PRINTF(last_size, "%s: the last send's size", name);
    for (int i = 0; i < RECEIVERS; i++) {
        PRINTF(result[i], "%s: %s's result", name, names[i]);
        PRINTF(moved[i], "%s: %s got \"%s\", size", name, names[i], received[i]);
    }
    PRINTF(CAPACITY - info.freeSize, "%s: the bytes left in the pipe", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);

    // A waits first at the lowest priority, B is above it and C above B
    const int rejoin_priorities[RECEIVERS] = { 0x90, 0x80, 0x78 };
    // A waits first at the highest priority, B is below it and C below B
    const int change_priorities[RECEIVERS] = { 0x78, 0x80, 0x90 };

    run("rejoin, A holds 4 of 8", REJOIN, 1, rejoin_priorities);
    run("priority change, A holds 4 of 8", CHANGE, 1, change_priorities);
    run("rejoin, A holds nothing", REJOIN, 0, rejoin_priorities);
    run("priority change, A holds nothing", CHANGE, 0, change_priorities);

    test_exit();
    return 0;
}
