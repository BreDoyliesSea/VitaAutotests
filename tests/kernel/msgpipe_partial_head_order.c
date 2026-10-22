// Where a new message pipe waiter queues when the head of the queue already got part of its bytes.
// The pipe has attr 0x2, which queues receivers by priority. Receiver A, of low priority, waits for
// 8 bytes FULL and queues first. Then receiver B, of high priority, waits for 4 bytes. This thread
// sends 4 bytes at a time with a try send, the second one once the first has ended a receive, so
// each send goes to the head at that moment. The bytes are letters, and the test prints per
// receiver its result, its size, and what it got as a string. Three cases:
// - A got "abcd" from a send of 4 before B queued, and B receives ASAP 4. Then sends of "efgh" and
//   "ijkl".
// - The same, with B receiving FULL 4.
// - A got nothing before B queued, and B receives ASAP 4. Then sends of "efgh" and "ijklmnop".
// The first two ask whether B queues in front of A or behind it, and the last is the reference for
// a head that moved nothing. A receive that still waits at the end is ended by canceling the pipe.
//
// On a PS Vita TV on firmware 3.65 B queues behind A while A holds part of its bytes, whatever B's
// mode, so the next send completes A and the one after goes to B. With A holding nothing, B queues
// in front of A and gets the first send.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096
#define A_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER + 2)
#define B_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER - 2)

static const char *const names[2] = { "A", "B" };

static SceUID pipe;
static SceUID threads[2];
static volatile int sizes[2];
static volatile SceUInt32 modes[2];
static volatile int result[2], done_count;
static SceSize moved[2];
static char received[2][16];

static int receiver(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    result[i] = sceKernelReceiveMsgPipe(pipe, received[i], sizes[i], modes[i], &moved[i], &timeout);
    __atomic_fetch_add(&done_count, 1, __ATOMIC_SEQ_CST);
    return 0;
}

// Starts receiver i on core 0 and waits until it waits
static void start(int i, int size, SceUInt32 mode, int priority) {
    result[i] = 0xDEAD;
    moved[i] = 0;
    sceClibMemset(received[i], 0, sizeof(received[i]));
    sizes[i] = size;
    modes[i] = mode;
    threads[i] = sceKernelCreateThread("receiver", receiver, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(threads[i], sizeof(i), &i);
    test_wait_until_waiting(threads[i]);
}

static void send(const char *bytes) {
    SceSize size;
    sceKernelTrySendMsgPipe(pipe, (void *)bytes, sceClibStrnlen(bytes, 16), MSG_PIPE_MODE_FULL, &size);
}

// Runs one case: A waits, then first is sent if there is one, then B waits, then second and third
static void run(const char *name, const char *first, SceUInt32 b_mode, const char *second, const char *third) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, MSG_PIPE_ATTR_RECEIVE_PRIO, CAPACITY, NULL);
    done_count = 0;
    start(0, 8, MSG_PIPE_MODE_FULL, A_PRIORITY);
    if (first) {
        send(first);
        // A waits for the other 4 bytes, so this reads right after the send that would have ended it
        PRINTF(done_count, "%s: receivers done after the send of \"%s\"", name, first);
    }
    start(1, 4, b_mode, B_PRIORITY);
    send(second);
    TEST_WAIT_FOR(done_count >= 1);
    send(third);
    TEST_WAIT_FOR(done_count >= 2);
    // Read before the cancel, which empties the pipe, so bytes left behind show
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    sceKernelCancelMsgPipe(pipe, NULL, NULL);
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);

    for (int i = 0; i < 2; i++) {
        PRINTF(result[i], "%s: %s's result", name, names[i]);
        PRINTF(moved[i], "%s: %s got \"%s\", size", name, names[i], received[i]);
    }
    PRINTF(CAPACITY - info.freeSize, "%s: the bytes left in the pipe", name);
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);

    run("A took 4 of 8, B ASAP 4", "abcd", MSG_PIPE_MODE_ASAP, "efgh", "ijkl");
    run("A took 4 of 8, B FULL 4", "abcd", MSG_PIPE_MODE_FULL, "efgh", "ijkl");
    run("A took nothing, B ASAP 4", NULL, MSG_PIPE_MODE_ASAP, "efgh", "ijklmnop");

    test_exit();
    return 0;
}
