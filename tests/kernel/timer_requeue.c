// Which waiter gets an auto reset timer's event after a priority change moved its waiters. Waiter A
// at 0x70 waits for EVENT_TIMER first and waiter B at 0x80 second, both on core 0 with a timeout of
// 2 s, and the main thread runs on core 1. Each case uses a fresh timer with a one shot set event
// 500 ms away. The main thread starts the timer once both wait, then changes a priority:
// - priority queue, A drops to 0x90
// - priority queue, B is raised to 0x60
// - FIFO queue, A drops to 0x90
// Each prints both waits' results. The one that doesn't get the event times out.
//
// On a PS Vita TV on firmware 3.65 the event goes to B in both priority queue cases, since the
// change put B first, and to A in the FIFO queue, where a priority change moves nobody.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD

static SceUID timer;
static volatile int results[2];

static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 pattern = 0;
    SceUInt64 user_data = 0;
    SceUInt32 timeout = 2000000;
    results[i] = sceKernelWaitEvent(timer, EVENT_TIMER, &pattern, &user_data, &timeout);
    return 0;
}

// Starts waiter i at priority on core 0 and returns once it waits on the timer
static SceUID start_waiter(int i, int priority) {
    results[i] = NOT_DONE;
    const SceUID thread = sceKernelCreateThread("waiter", waiter_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_EVENT, timer));
    return thread;
}

// Runs one case: changed is 0 for A and 1 for B, which gets priority
static void requeue_case(const char *name, SceUInt32 order, int changed, int priority) {
    timer = sceKernelCreateTimer("timer", EVENT_ATTR_AUTO_RESET | order, NULL);
    SceUInt64 interval = 500000;
    sceKernelSetTimerEvent(timer, TIMER_TYPE_SET_EVENT, &interval, 0);
    // Separate statements, so A queues before B
    SceUID threads[2];
    threads[0] = start_waiter(0, 0x70);
    threads[1] = start_waiter(1, 0x80);
    sceKernelStartTimer(timer);
    const int change = sceKernelChangeThreadPriority(threads[changed], priority);
    test_join_thread(threads[0]);
    test_join_thread(threads[1]);
    PRINTF(change, "%s: the priority change", name);
    PRINTF(results[0], "%s: A's wait", name);
    PRINTF(results[1], "%s: B's wait", name);
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    requeue_case("priority queue, A drops to 0x90", SCE_KERNEL_ATTR_THREAD_PRIO, 0, 0x90);
    requeue_case("priority queue, B is raised to 0x60", SCE_KERNEL_ATTR_THREAD_PRIO, 1, 0x60);
    requeue_case("FIFO queue, A drops to 0x90", 0, 0, 0x90);

    test_exit();
    return 0;
}
