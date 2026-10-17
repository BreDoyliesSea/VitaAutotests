// Which waiting message pipe receiver gets the next bytes after a priority change moves a receiver
// that already got part of a FULL receive. Receiver A waits for 8 bytes FULL and queues first,
// then B waits for 4 bytes ASAP behind it, and a send of "abcd" gives A its first 4 bytes. Then
// one priority change, and two more sends of 4 bytes, "efgh" and "ijkl", the second once the
// first has woken a receiver:
// - attr 0x2, which queues receivers by priority: A drops below B
// - attr 0x2: B is raised above A
// - attr 0x0, FIFO: A drops below B
// Each case prints how many receivers were done within 500 ms of the first send and of the
// change, which receiver each later send woke, as 0xA or 0xB, and what each one got. A receive
// that doesn't end by then ends when the pipe is deleted.
//
// On a PS Vita TV on firmware 3.65 neither "abcd" nor the change ends a receive. With attr 0x2, A
// dropping below B lets B take "efgh", and A ends with "abcdijkl". B raised above A doesn't: A,
// which already holds "abcd", still takes "efgh" first, and B gets "ijkl". The FIFO pipe gives A
// "abcdefgh" and B "ijkl".
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096
#define HIGH 0x78
#define A_PRIORITY 0x80
#define B_PRIORITY 0x88
#define LOW 0x90

static const int sizes[2] = { 8, 4 };
static const SceUInt32 modes[2] = { MSG_PIPE_MODE_FULL, MSG_PIPE_MODE_ASAP };
static const char *const names[2] = { "A", "B" };

static SceUID pipe;
static SceUID threads[2];
static volatile int result[2], done_count, woke[2];
static SceSize moved[2];
static char received[2][16];

static int receiver(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    result[i] = sceKernelReceiveMsgPipe(pipe, received[i], sizes[i], modes[i], &moved[i], &timeout);
    const int n = __atomic_fetch_add(&done_count, 1, __ATOMIC_SEQ_CST);
    if (n < 2)
        woke[n] = 0xA + i;
    return 0;
}

// Waits up to 500 ms for a receiver to be done, for an outcome that would come at once
static int done_soon(void) {
    for (int t = 0; t < 500 && done_count == 0; t++)
        sceKernelDelayThread(1000);
    return done_count;
}

static void send(const char *bytes) {
    SceSize size;
    sceKernelTrySendMsgPipe(pipe, (void *)bytes, 4, MSG_PIPE_MODE_FULL, &size);
}

static void run(const char *name, SceUInt32 attr, int who, int priority) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, attr, CAPACITY, NULL);
    done_count = 0;
    static const int priorities[2] = { A_PRIORITY, B_PRIORITY };
    for (int i = 0; i < 2; i++) {
        result[i] = 0xDEAD;
        woke[i] = 0;
        moved[i] = 0;
        sceClibMemset(received[i], 0, sizeof(received[i]));
        threads[i] = sceKernelCreateThread("receiver", receiver, priorities[i], 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }

    send("abcd");
    const int after_first = done_soon();
    const int changed = sceKernelChangeThreadPriority(threads[who], priority);
    const int after_change = done_soon();
    send("efgh");
    TEST_WAIT_FOR(done_count >= 1);
    send("ijkl");
    TEST_WAIT_FOR(done_count >= 2);
    sceKernelDeleteMsgPipe(pipe);
    // Joined before the records are read, since a receiver counts itself before it writes its own
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);

    PRINTF(after_first, "%s: receivers done after \"abcd\"", name);
    PRINTF(changed, "%s: the change", name);
    PRINTF(after_change, "%s: receivers done after the change", name);
    PRINTF(woke[0], "%s: the receiver \"efgh\" woke", name);
    PRINTF(woke[1], "%s: the receiver \"ijkl\" woke", name);
    for (int i = 0; i < 2; i++) {
        PRINTF(result[i], "%s: %s's result", name, names[i]);
        PRINTF(moved[i], "%s: %s got \"%s\", size", name, names[i], received[i]);
    }
}

int main(void) {
    run("attr 0x2, A drops below B", 0x2, 0, LOW);
    run("attr 0x2, B raised above A", 0x2, 1, HIGH);
    run("attr 0x0, A drops below B", 0x0, 0, LOW);

    test_exit();
    return 0;
}
