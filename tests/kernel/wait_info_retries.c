// What sceKernelGetThreadInfo reports for a waiting thread after its object changed in a way that
// doesn't end its wait: an event flag waiter for bits it didn't get, a semaphore waiter that needs
// more than was signaled, a message pipe receiver that wants more than was sent, and a mutex waiter
// behind the one that got the mutex. Then what a waiter reports once its wait ended but before it
// ran again. Every thread is pinned to core 0 and the waiters have a lower priority than the main
// thread, so a waiter the kernel made ready can't run and change what it reports until the main
// thread blocks. Each state is read first and printed later, since printing lets the waiters run.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// Lower than the main thread's 0xA0
#define LOW_PRIORITY 0xB0
#define PIPE_SIZE 4096

static SceUID flag, sema, pipe, mutex;
static volatile int result[2];
static unsigned int flag_bits;
static unsigned int received;
static char pipe_buf[8];

typedef struct State {
    int status, wait_type, on_object;
} State;

// Reads what thread reports about its wait, and whether it waits on object
static State state_of(SceUID thread, SceUID object) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return (State){ .status = info.status, .wait_type = info.waitType, .on_object = info.waitId == object };
}

static void print_state(const char *name, State state) {
    PRINTF(state.status, "%s: its status", name);
    PRINTF(state.wait_type, "%s: its wait type", name);
    PRINTF(state.on_object, "%s: it reports the object as what it waits on", name);
}

static int flag_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    result[0] = sceKernelWaitEventFlag(flag, 0x3, SCE_EVENT_WAITAND, &flag_bits, &timeout);
    return 0;
}

static int sema_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    result[0] = sceKernelWaitSema(sema, 3, &timeout);
    return 0;
}

static int pipe_receiver(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    result[0] = sceKernelReceiveMsgPipe(pipe, pipe_buf, sizeof(pipe_buf), MSG_PIPE_MODE_FULL, &received, &timeout);
    return 0;
}

// Locks the mutex into slot i, and unlocks it again if it got it
static int mutex_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    result[i] = sceKernelLockMutex(mutex, 1, &timeout);
    if (result[i] == 0)
        sceKernelUnlockMutex(mutex, 1);
    return 0;
}

// Starts a low priority waiter on core 0 and returns once it waits
static SceUID start_waiter(SceKernelThreadEntry entry, int i) {
    result[i] = -1;
    const SceUID thread = sceKernelCreateThread("waiter", entry, LOW_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    test_wait_until_waiting(thread);
    return thread;
}

static void event_flag_case(void) {
    flag = sceKernelCreateEventFlag("flag", 0, 0, NULL);
    flag_bits = 0xDEAD;
    const SceUID waiter = start_waiter(flag_waiter, 0);
    const State waiting = state_of(waiter, flag);
    sceKernelSetEventFlag(flag, 0x1);
    const State one_bit = state_of(waiter, flag);
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(flag, &info);
    // The bit it got goes again before the other one comes
    sceKernelClearEventFlag(flag, ~0x1u);
    sceKernelSetEventFlag(flag, 0x2);
    const State other_bit = state_of(waiter, flag);
    sceKernelSetEventFlag(flag, 0x1);
    const State woken = state_of(waiter, flag);
    test_join_thread(waiter);

    print_state("event flag, waiting for 0x3", waiting);
    print_state("event flag, after a set of 0x1", one_bit);
    PRINT("event flag, after a set of 0x1: the flag's waiting threads", info.numWaitThreads);
    print_state("event flag, after a clear of 0x1 and a set of 0x2", other_bit);
    print_state("event flag, after a set of 0x1 again", woken);
    PRINT("event flag: the wait's result", result[0]);
    PRINT("event flag: the bits it got", flag_bits);
    sceKernelDeleteEventFlag(flag);
}

static void sema_case(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 5, NULL);
    SceKernelSemaInfo info[3];
    for (int i = 0; i < 3; i++)
        info[i] = (SceKernelSemaInfo){ .size = sizeof(info[i]) };
    const SceUID waiter = start_waiter(sema_waiter, 0);
    const State waiting = state_of(waiter, sema);
    sceKernelSignalSema(sema, 1);
    const State signaled_1 = state_of(waiter, sema);
    sceKernelGetSemaInfo(sema, &info[0]);
    sceKernelSignalSema(sema, 1);
    const State signaled_2 = state_of(waiter, sema);
    sceKernelGetSemaInfo(sema, &info[1]);
    sceKernelSignalSema(sema, 2);
    const State woken = state_of(waiter, sema);
    sceKernelGetSemaInfo(sema, &info[2]);
    test_join_thread(waiter);

    print_state("semaphore, waiting for 3", waiting);
    print_state("semaphore, after a signal of 1", signaled_1);
    PRINT("semaphore, after a signal of 1: the count", info[0].currentCount);
    PRINT("semaphore, after a signal of 1: the waiting threads", info[0].numWaitThreads);
    print_state("semaphore, after another signal of 1", signaled_2);
    PRINT("semaphore, after another signal of 1: the count", info[1].currentCount);
    PRINT("semaphore, after another signal of 1: the waiting threads", info[1].numWaitThreads);
    print_state("semaphore, after a signal of 2", woken);
    PRINT("semaphore, after a signal of 2: the count", info[2].currentCount);
    PRINT("semaphore, after a signal of 2: the waiting threads", info[2].numWaitThreads);
    PRINT("semaphore: the wait's result", result[0]);
    sceKernelDeleteSema(sema);
}

static void pipe_case(void) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, PIPE_SIZE, NULL);
    SceKernelMppInfo info[2];
    for (int i = 0; i < 2; i++)
        info[i] = (SceKernelMppInfo){ .size = sizeof(info[i]) };
    SceSize size;
    received = 0xDEAD;
    sceClibMemset(pipe_buf, 0, sizeof(pipe_buf));
    const SceUID waiter = start_waiter(pipe_receiver, 0);
    const State waiting = state_of(waiter, pipe);
    const int first_send = sceKernelTrySendMsgPipe(pipe, "abcd", 4, MSG_PIPE_MODE_FULL, &size);
    const State half = state_of(waiter, pipe);
    sceKernelGetMsgPipeInfo(pipe, &info[0]);
    const int second_send = sceKernelTrySendMsgPipe(pipe, "efgh", 4, MSG_PIPE_MODE_FULL, &size);
    const State woken = state_of(waiter, pipe);
    sceKernelGetMsgPipeInfo(pipe, &info[1]);
    test_join_thread(waiter);

    print_state("message pipe, receiving 8 bytes", waiting);
    PRINT("message pipe: send 4 bytes", first_send);
    print_state("message pipe, after 4 bytes", half);
    PRINT("message pipe, after 4 bytes: the bytes in the pipe", PIPE_SIZE - info[0].freeSize);
    PRINT("message pipe, after 4 bytes: the waiting receivers", info[0].numReceiveWaitThreads);
    PRINT("message pipe: send 4 more", second_send);
    print_state("message pipe, after 4 more", woken);
    PRINT("message pipe, after 4 more: the bytes in the pipe", PIPE_SIZE - info[1].freeSize);
    PRINT("message pipe, after 4 more: the waiting receivers", info[1].numReceiveWaitThreads);
    PRINT("message pipe: the receive's result", result[0]);
    PRINT("message pipe: the bytes it received", received);
    PRINT("message pipe: they arrived in order", sceClibMemcmp(pipe_buf, "abcdefgh", 8) == 0);
    sceKernelDeleteMsgPipe(pipe);
}

static void mutex_case(void) {
    mutex = sceKernelCreateMutex("mutex", 0, 1, NULL);
    const SceUID first = start_waiter(mutex_waiter, 0);
    const SceUID second = start_waiter(mutex_waiter, 1);
    const State first_waiting = state_of(first, mutex);
    sceKernelUnlockMutex(mutex, 1);
    const State first_woken = state_of(first, mutex);
    const State second_waiting = state_of(second, mutex);
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    test_join_thread(first);
    test_join_thread(second);

    print_state("mutex, the first waiter", first_waiting);
    print_state("mutex, the first waiter after the unlock", first_woken);
    print_state("mutex, the second waiter after the unlock", second_waiting);
    PRINT("mutex, after the unlock: the first waiter owns it", info.currentOwnerId == first);
    PRINT("mutex, after the unlock: its count", info.currentCount);
    PRINT("mutex, after the unlock: the waiting threads", info.numWaitThreads);
    PRINT("mutex: the first waiter's lock", result[0]);
    PRINT("mutex: the second waiter's lock", result[1]);
    sceKernelDeleteMutex(mutex);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    event_flag_case();
    sema_case();
    pipe_case();
    mutex_case();

    test_exit();
    return 0;
}
