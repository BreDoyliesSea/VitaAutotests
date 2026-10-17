// What a thread waiting for T's end gets when T's end woke it, but T is started again or deleted
// before the waiter runs: the status of the run it waited for, or what T holds when the waiter
// returns. Every thread is pinned to core 0. T runs at 0x50, above the main thread at 0x60, and
// waits for go, then ends with the status it was started with. End waiter W, at 0xB0, waits for
// T's end, so once that wakes it, W only runs when the main thread blocks. The cases:
// - control: T ends with 0x111
// - T ends with 0x111, then the main thread starts it again, so it waits for go once more
// - T ends with 0x111, is started again and ends with 0x222
// - T ends with 0x111, then the main thread deletes it
// - W2, at 0xB0, waits for T's end twice: T ends with 0x111 and is started again before W2 runs,
//   then ends with 0x222 once W2 waits again
// Each case prints W's result and the status it got, read once W ended. The last case prints both
// results of W2 and the status each one got.
//
// On a PS Vita TV on firmware 3.65 W gets 0x111, the status of the run it waited for, whether T is
// started again, started and ended again, or deleted before W runs. W2 waiting again after the
// start gets the second run's 0x222.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define T_PRIORITY 0x50
#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0

static SceUID go, target;
static volatile int end_result[2], end_status[2];
static volatile int waits_twice;

static int target_entry(SceSize args, void *argp) {
    (void)args;
    const int status = *(const int *)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(go, 1, &timeout);
    return status;
}

static int end_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const int count = waits_twice ? 2 : 1;
    for (int i = 0; i < count; i++) {
        int status = 0xDEAD;
        SceUInt timeout = 3 * TEST_WAIT_US;
        end_result[i] = sceKernelWaitThreadEnd(target, &status, &timeout);
        end_status[i] = status;
    }
    return 0;
}

static void start_target(int status) {
    sceKernelStartThread(target, sizeof(status), &status);
}

// Waits for T's end. On a Vita T ended before the signal returned, so this returns at once without
// blocking, and W can't run in the meantime.
static void wait_until_ended(void) {
    sceKernelWaitThreadEnd(target, NULL, NULL);
}

typedef enum Case { CONTROL, RESTART, RESTART_END, DELETE } Case;

static void run_case(const char *name, Case c) {
    waits_twice = 0;
    end_result[0] = end_status[0] = 0xDEAD;
    target = sceKernelCreateThread(
        "t", target_entry, T_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    // T runs at once, since it is above the main thread, until it waits for go
    start_target(0x111);
    const SceUID waiter = sceKernelCreateThread(
        "w", end_waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(waiter, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(waiter, WAIT_TYPE_THREAD_END, target));
    // T takes the core from the main thread and ends before the signal returns
    sceKernelSignalSema(go, 1);
    wait_until_ended();
    int step = 0;
    if (c == RESTART || c == RESTART_END)
        start_target(0x222);
    if (c == RESTART_END) {
        sceKernelSignalSema(go, 1);
        wait_until_ended();
    }
    if (c == DELETE)
        step = sceKernelDeleteThread(target);
    // The main thread blocks here for the first time since T's end, so W runs
    test_join_thread(waiter);
    if (c == DELETE)
        PRINTF(step, "%s: delete T", name);
    PRINTF(end_result[0], "%s: W's result", name);
    PRINTF(end_status[0], "%s: the status W got", name);
    if (c == RESTART)
        sceKernelSignalSema(go, 1);
    if (c != DELETE) {
        sceKernelWaitThreadEnd(target, NULL, NULL);
        sceKernelDeleteThread(target);
    }
}

// W2 waits for T's end twice. Its first wait ends with T's first end, and its second one waits for
// the end of the run the main thread starts before W2 runs.
static void twice_case(void) {
    const char *name = "W2 waits again after a start";
    waits_twice = 1;
    end_result[0] = end_status[0] = end_result[1] = end_status[1] = 0xDEAD;
    target = sceKernelCreateThread(
        "t", target_entry, T_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    start_target(0x111);
    const SceUID waiter = sceKernelCreateThread(
        "w2", end_waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(waiter, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(waiter, WAIT_TYPE_THREAD_END, target));
    sceKernelSignalSema(go, 1);
    wait_until_ended();
    start_target(0x222);
    // W2 runs now, and waits for the second run's end
    TEST_WAIT_FOR(
        end_result[0] != (int)0xDEAD && test_in_wait(waiter, WAIT_TYPE_THREAD_END, target));
    const int waits_again = test_in_wait(waiter, WAIT_TYPE_THREAD_END, target);
    sceKernelSignalSema(go, 1);
    test_join_thread(waiter);
    PRINTF(end_result[0], "%s: W2's first result", name);
    PRINTF(end_status[0], "%s: the status it got", name);
    PRINTF(waits_again, "%s: W2 waits again for the second run", name);
    PRINTF(end_result[1], "%s: W2's second result", name);
    PRINTF(end_status[1], "%s: the status it got", name);
    sceKernelWaitThreadEnd(target, NULL, NULL);
    sceKernelDeleteThread(target);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);

    run_case("control", CONTROL);
    run_case("started again", RESTART);
    run_case("started again and ended again", RESTART_END);
    run_case("deleted", DELETE);
    twice_case();

    sceKernelDeleteSema(go);
    test_exit();
    return 0;
}
