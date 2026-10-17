// Whether a priority change that moves waiting message pipe senders around lets the ones that now
// fit put their bytes in, without any receive. The pipe holds 4096 bytes and is filled with '.' up
// to 8 free bytes. Sender A sends 10 bytes FULL, which don't fit, and queues first, then B and C
// send 2 bytes FULL each, which would fit, and queue behind it. Then one priority change:
// - attr 0x1, which queues senders by priority: A drops below B and C
// - attr 0x1: C is raised above A
// - attr 0x1, A and B at the same priority: A is set to the priority it has
// - attr 0x0 and attr 0x2, whose senders queue in FIFO order: A drops below B and C
// Each case prints the free size and the number of waiting senders before and right after the
// change, and which senders were done within 500 ms of it. Then the pipe is canceled, which ends
// the sends left, and the test receives what the pipe holds and prints it without the '.' bytes. No
// receive runs while a sender waits.
//
// On a PS Vita TV on firmware 3.65 with attr 0x1 the change lets the senders that now come first
// put their bytes in at once, up to the first one that doesn't fit: A dropping below B and C lets
// both in, which leaves 4 bytes free, C raised above A lets C in, and A set to its own priority
// behind an equal B lets B in. With attr 0x0 and 0x2 nothing happens. The cancel ends the sends
// left with WAIT_CANCEL (0x80028007), and the receive after it finds the pipe empty (0x80028223,
// MSG_PIPE_EMPTY), so the cancel also drops what the pipe held.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096
#define FREE 8
#define HIGH 0x78
#define A_PRIORITY 0x80
#define B_PRIORITY 0x88
#define C_PRIORITY 0x90
#define LOW 0x98

static const char *const data[3] = { "abcdefghij", "kl", "mn" };
static const char *const names[3] = { "A", "B", "C" };

static SceUID pipe;
static SceUID threads[3];
static volatile int result[3], done[3];
static SceSize moved[3];
static char fill[CAPACITY], drained[CAPACITY];

static int sender(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    result[i] = sceKernelSendMsgPipe(pipe, (void *)data[i], sceClibStrnlen(data[i], 16),
                                     MSG_PIPE_MODE_FULL, &moved[i], &timeout);
    done[i] = 1;
    return 0;
}

static void start(SceUInt32 attr, const int *priorities) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, attr, CAPACITY, NULL);
    sceKernelTrySendMsgPipe(pipe, fill, CAPACITY - FREE, MSG_PIPE_MODE_FULL, NULL);
    for (int i = 0; i < 3; i++) {
        result[i] = 0xDEAD;
        done[i] = 0;
        moved[i] = 0;
        threads[i] = sceKernelCreateThread("sender", sender, priorities[i], 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }
}

// Waits up to 500 ms for B and C to be done, for an outcome that would come at once
static void settle(void) {
    for (int t = 0; t < 500 && !(done[1] && done[2]); t++)
        sceKernelDelayThread(1000);
}

static void change_case(const char *name, SceUInt32 attr, const int *priorities, int who,
                        int priority) {
    start(attr, priorities);
    SceKernelMppInfo before = { .size = sizeof(before) };
    sceKernelGetMsgPipeInfo(pipe, &before);
    const int changed = sceKernelChangeThreadPriority(threads[who], priority);
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    settle();
    PRINTF(before.freeSize, "%s: the free size before the change", name);
    PRINTF(before.numSendWaitThreads, "%s: the waiting senders before the change", name);
    PRINTF(changed, "%s: the change", name);
    PRINTF(info.freeSize, "%s: the free size right after the change", name);
    PRINTF(info.numSendWaitThreads, "%s: the waiting senders right after the change", name);
    for (int i = 0; i < 3; i++)
        PRINTF(done[i], "%s: %s is done after the change", name, names[i]);

    int waiting = -1, receivers = -1;
    const int canceled = sceKernelCancelMsgPipe(pipe, &waiting, &receivers);
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);
    PRINTF(canceled, "%s: cancel the pipe", name);
    PRINTF(waiting, "%s: the senders the cancel ended", name);
    for (int i = 0; i < 3; i++) {
        PRINTF(result[i], "%s: %s's result", name, names[i]);
        PRINTF(moved[i], "%s: %s's size sent", name, names[i]);
    }

    char seen[32];
    int seen_len = 0;
    SceSize size = 0;
    const int received = sceKernelTryReceiveMsgPipe(pipe, drained, CAPACITY, MSG_PIPE_MODE_ASAP,
                                                    &size);
    for (SceSize i = 0; received == 0 && i < size; i++) {
        if (drained[i] != '.' && seen_len < (int)sizeof(seen) - 1)
            seen[seen_len++] = drained[i];
    }
    seen[seen_len] = '\0';
    PRINTF(received, "%s: receive what the pipe holds", name);
    PRINTF(seen_len, "%s: the pipe held \"%s\" past the '.' bytes", name, seen);
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    sceClibMemset(fill, '.', sizeof(fill));

    static const int a_b_c[3] = { A_PRIORITY, B_PRIORITY, C_PRIORITY };
    static const int a_with_b[3] = { A_PRIORITY, A_PRIORITY, C_PRIORITY };

    change_case("attr 0x1, A drops below B and C", 0x1, a_b_c, 0, LOW);
    change_case("attr 0x1, C raised above A", 0x1, a_b_c, 2, HIGH);
    change_case("attr 0x1, A set to its own priority", 0x1, a_with_b, 0, A_PRIORITY);
    change_case("attr 0x0, A drops below B and C", 0x0, a_b_c, 0, LOW);
    change_case("attr 0x2, A drops below B and C", 0x2, a_b_c, 0, LOW);

    test_exit();
    return 0;
}
