// When a thread made ready from elsewhere takes the core from a running one: thread L runs on core
// 0 in a loop without syscalls while thread O on core 0 becomes ready, woken by a signal from the
// main thread on core 1 or by the end of its own delay. Then L makes a wait call that doesn't have
// to wait. O has a higher priority than L, or the same. The threads record their steps as small
// numbers, printed in the order they were recorded: L records 0 before its loop, then 2 if O ran
// during the loop or 3 if it didn't, then 4 after its call. O records 1 when it runs. So 0 1 2 4
// means O took the core during the loop, 0 3 1 4 during the call, and 0 3 4 1 only after L ended.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define HIGH_PRIORITY 0x80
#define LOW_PRIORITY 0xA0
// About a second on a Vita, far longer than the main thread takes to wake O
#define SPINS 150000000
#define MAX_STEPS 6

typedef enum Wake { SIGNAL, DELAY } Wake;
typedef enum Call { WAIT_SEMA, POLL_SEMA, WAIT_FLAG } Call;

static SceUID wake_sema, free_sema, set_flag;
static volatile int spinning, other_ran, call_result;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static int other_entry(SceSize args, void *argp) {
    (void)args;
    const Wake wake = *(const Wake *)argp;
    if (wake == SIGNAL) {
        SceUInt timeout = 2 * TEST_WAIT_US;
        sceKernelWaitSema(wake_sema, 1, &timeout);
    } else {
        sceKernelDelayThread(50000);
    }
    record(1);
    other_ran = 1;
    return 0;
}

static int low_entry(SceSize args, void *argp) {
    (void)args;
    const Call call = *(const Call *)argp;
    record(0);
    spinning = 1;
    for (volatile int n = 0; n < SPINS && !other_ran; n++)
        ;
    record(other_ran ? 2 : 3);
    SceUInt timeout = TEST_WAIT_US;
    SceUInt32 bits;
    switch (call) {
    case WAIT_SEMA: call_result = sceKernelWaitSema(free_sema, 1, &timeout); break;
    case POLL_SEMA: call_result = sceKernelPollSema(free_sema, 1); break;
    case WAIT_FLAG: call_result = sceKernelWaitEventFlag(set_flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    }
    record(4);
    return 0;
}

static void run_case(const char *name, int other_priority, Wake wake, Call call) {
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
    spinning = other_ran = 0;
    call_result = 0xDEAD;
    wake_sema = sceKernelCreateSema("wake", 0, 0, 1, NULL);
    free_sema = sceKernelCreateSema("free", 0, 1, 1, NULL);
    set_flag = sceKernelCreateEventFlag("flag", 0, 0x1, NULL);

    const SceUID other = sceKernelCreateThread("other", other_entry, other_priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(other, sizeof(wake), &wake);
    // O waits on the semaphore or in its delay before L starts
    test_wait_until_waiting(other);
    const SceUID low = sceKernelCreateThread("low", low_entry, LOW_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(low, sizeof(call), &call);
    TEST_WAIT_FOR(spinning);
    if (wake == SIGNAL)
        sceKernelSignalSema(wake_sema, 1);
    sceKernelWaitThreadEnd(low, NULL, NULL);
    sceKernelWaitThreadEnd(other, NULL, NULL);
    sceKernelDeleteThread(low);
    sceKernelDeleteThread(other);
    sceKernelDeleteSema(wake_sema);
    sceKernelDeleteSema(free_sema);
    sceKernelDeleteEventFlag(set_flag);

    PRINTF(call_result, "%s: L's call", name);
    const int count = step_count < MAX_STEPS ? step_count : MAX_STEPS;
    for (int i = 0; i < count; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("higher priority, signaled from core 1, then a wait on a free semaphore", HIGH_PRIORITY, SIGNAL, WAIT_SEMA);
    run_case("higher priority, its delay ends, then a wait on a free semaphore", HIGH_PRIORITY, DELAY, WAIT_SEMA);
    run_case("same priority, signaled from core 1, then a wait on a free semaphore", LOW_PRIORITY, SIGNAL, WAIT_SEMA);
    run_case("same priority, signaled from core 1, then a poll of a free semaphore", LOW_PRIORITY, SIGNAL, POLL_SEMA);
    run_case("same priority, signaled from core 1, then a wait on a set event flag", LOW_PRIORITY, SIGNAL, WAIT_FLAG);
    run_case("same priority, its delay ends, then a wait on a free semaphore", LOW_PRIORITY, DELAY, WAIT_SEMA);

    test_exit();
    return 0;
}
