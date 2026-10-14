// Which waiting receiver of a message pipe gets which bytes, and in which order waiting senders get
// their bytes in, for each queue order attribute a pipe takes. Two threads wait at a time, one of
// low priority that queues first and one of high priority that queues second, with different sizes
// and modes. Thread 1 is the low priority one. The bytes are letters, so the test prints what each
// receiver got and what the pipe gave back as short strings. A pipe that must be full is filled
// with '.' bytes first. Left out: receives from a full pipe that leave a FULL sender waiting. On a
// PS Vita TV on firmware 3.65 such a receive never returned and the console stopped responding.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096
#define LOW_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER + 2)
#define HIGH_PRIORITY (SCE_KERNEL_DEFAULT_PRIORITY_USER - 2)

typedef struct Op {
    int send;
    SceUInt32 mode;
    int size;
    const char *data;
} Op;

static SceUID pipe;
static Op ops[2];
static SceUID threads[2];
static volatile int result[2], done[2];
static SceSize moved[2];
static char received[2][16];
static char fill[CAPACITY], drained[CAPACITY];
static char seen[64];
static int seen_len;

// The type of the wait thread is in, without the callback bit, or 0 if it doesn't wait
static SceUInt32 wait_type(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0 || !(info.status & SCE_THREAD_WAITING))
        return 0;
    return info.waitType & ~WAIT_TYPE_CB;
}

static int waits_in(SceUID thread, SceUInt32 type) {
    return wait_type(thread) == type;
}

static int op_thread(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    if (ops[i].send)
        result[i] = sceKernelSendMsgPipe(pipe, (void *)ops[i].data, ops[i].size, ops[i].mode, &moved[i], &timeout);
    else
        result[i] = sceKernelReceiveMsgPipe(pipe, received[i], ops[i].size, ops[i].mode, &moved[i], &timeout);
    done[i] = 1;
    return 0;
}

// Starts ops[0] at low priority, then ops[1] at high priority, each once the one before waits
static void start_ops(const Op *list) {
    for (int i = 0; i < 2; i++) {
        ops[i] = list[i];
        result[i] = 0xDEAD;
        done[i] = 0;
        moved[i] = 0;
        sceClibMemset(received[i], 0, sizeof(received[i]));
        threads[i] = sceKernelCreateThread("op", op_thread, i == 0 ? LOW_PRIORITY : HIGH_PRIORITY, 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }
}

static int settled(void) {
    return (done[0] || waits_in(threads[0], WAIT_TYPE_MSG_PIPE)) && (done[1] || waits_in(threads[1], WAIT_TYPE_MSG_PIPE));
}

// Waits until each op is done or waits on the pipe, on 20 polls in a row, at most 5 s. A thread
// that was just woken may show its wait until it runs, so one look isn't enough.
static void settle(void) {
    int stable = 0;
    for (int poll = 0; poll < 5000 && stable < 20; poll++) {
        stable = settled() ? stable + 1 : 0;
        if (stable < 20)
            sceKernelDelayThread(1000);
    }
}

// Receives what the pipe holds without waiting, and adds the bytes other than '.' to seen
static void drain(void) {
    SceSize size = 0;
    if (sceKernelTryReceiveMsgPipe(pipe, drained, CAPACITY, MSG_PIPE_MODE_ASAP, &size) < 0)
        return;
    for (SceSize i = 0; i < size; i++) {
        if (drained[i] != '.' && seen_len < (int)sizeof(seen) - 1)
            seen[seen_len++] = drained[i];
    }
    seen[seen_len] = '\0';
}

static void end_pipe(void) {
    sceKernelDeleteMsgPipe(pipe);
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);
}

static void print_receivers(const char *name) {
    for (int i = 0; i < 2; i++) {
        const char *who = i == 0 ? "receiver 1, low priority," : "receiver 2, high priority,";
        if (done[i])
            PRINTF(moved[i], "%s: %s gets \"%s\", size", name, who, received[i]);
        else
            PRINTF(1, "%s: %s still waits", name, who);
    }
}

// Two receivers wait on an empty pipe, then this thread sends each of sends in turn
static void receivers(SceUInt32 attr, const char *name, const Op *list, const char *const *sends, int count) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, attr, CAPACITY, NULL);
    start_ops(list);
    for (int s = 0; s < count; s++) {
        sceKernelTrySendMsgPipe(pipe, (void *)sends[s], sceClibStrnlen(sends[s], 16), MSG_PIPE_MODE_FULL, NULL);
        settle();
    }
    print_receivers(name);
    seen_len = 0;
    seen[0] = '\0';
    drain();
    PRINTF(seen_len, "%s: the pipe still holds \"%s\"", name, seen);
    end_pipe();
}

// Two senders wait on a full pipe, then this thread receives room bytes, and then the rest
static void senders(SceUInt32 attr, const char *name, const Op *list, int room) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, attr, CAPACITY, NULL);
    sceKernelTrySendMsgPipe(pipe, fill, CAPACITY, MSG_PIPE_MODE_FULL, NULL);
    start_ops(list);
    SceSize size;
    sceKernelTryReceiveMsgPipe(pipe, drained, room, MSG_PIPE_MODE_FULL, &size);
    settle();
    PRINTF(done[0], "%s: with room for %d sender 1, low priority, is done", name, room);
    PRINTF(done[1], "%s: with room for %d sender 2, high priority, is done", name, room);
    seen_len = 0;
    seen[0] = '\0';
    for (int i = 0; i < 3; i++) {
        drain();
        settle();
    }
    PRINTF(seen_len, "%s: the pipe gave back \"%s\"", name, seen);
    end_pipe();
}

int main(void) {
    sceClibMemset(fill, '.', sizeof(fill));

    static const Op full_3_2[2] = { { 0, MSG_PIPE_MODE_FULL, 3, NULL }, { 0, MSG_PIPE_MODE_FULL, 2, NULL } };
    static const Op asap_4_full_2[2] = { { 0, MSG_PIPE_MODE_ASAP, 4, NULL }, { 0, MSG_PIPE_MODE_FULL, 2, NULL } };
    static const Op full_3_3[2] = { { 0, MSG_PIPE_MODE_FULL, 3, NULL }, { 0, MSG_PIPE_MODE_FULL, 3, NULL } };
    static const Op send_3_2[2] = { { 1, MSG_PIPE_MODE_FULL, 3, "abc" }, { 1, MSG_PIPE_MODE_FULL, 2, "de" } };
    static const char *const one_send[] = { "ABCDE" };
    static const char *const two_sends[] = { "ABCD", "EF" };

    static const SceUInt32 attrs[] = { 0x0, 0x1, 0x2, 0x4, 0x8 };
    for (unsigned int a = 0; a < sizeof(attrs) / sizeof(attrs[0]); a++) {
        char name[96];
        sceClibSnprintf(name, sizeof(name), "attr 0x%X, receives FULL 3 and FULL 2, send 5", attrs[a]);
        receivers(attrs[a], name, full_3_2, one_send, 1);
        sceClibSnprintf(name, sizeof(name), "attr 0x%X, receives ASAP 4 and FULL 2, send 5", attrs[a]);
        receivers(attrs[a], name, asap_4_full_2, one_send, 1);
        sceClibSnprintf(name, sizeof(name), "attr 0x%X, receives FULL 3 and FULL 3, sends 4 and 2", attrs[a]);
        receivers(attrs[a], name, full_3_3, two_sends, 2);
        sceClibSnprintf(name, sizeof(name), "attr 0x%X, sends FULL 3 and FULL 2", attrs[a]);
        senders(attrs[a], name, send_3_2, CAPACITY);
    }

    test_exit();
    return 0;
}
