// What a thread that ran outside its mask does next. As in shared_ready_raise, a spinner thread
// at 0x40 holds core 0 without syscalls, the main thread runs on core 1 at 0x50, and T, at 0xA0
// on core 0 alone, is started from core 1, so it waits READY in the queue the cores share. The
// main thread raises T to 0x60 and lets core 1 go, and T notes its current CPU and its mask once
// it runs. Then, by case:
// - T waits on a semaphore, which the main thread signals while the spinner still holds core 0
// - the same, with the spinner ended before the signal
// - T delays itself 10 ms while the spinner still holds core 0
// - T lowers itself to 0xA0, the priority it started at, while the spinner still holds core 0
// While T waits on the semaphore, the main thread notes T's status, current and last CPU, mask
// and priority, and 10 ms after the signal, while it holds core 1, T's status and current CPU.
// Each case prints those, whether T ran within 200 ms of the raise, whether it went on within
// 200 ms of the signal, the delay or the change, and the core it went on on. The spinner ends at
// once when T didn't run beside it, and a T that no core takes once the spinner ended gets every
// core.
//
// On a PS Vita TV on firmware 3.65 T runs on core 1 at 0x60 after the raise, outside its mask,
// which T and the main thread still read as core 0 alone. While T waits, its info reports WAITING,
// current CPU -1, last CPU 1, that mask and 0x60. A signal while core 0 is held makes T STANDBY on
// core 0, and it goes on there once core 0 is free, and a signal once core 0 is free runs T there
// at once. A delay ending while core 0 is held also waits for core 0. Only T lowering itself to
// 0xA0 while it runs keeps it on core 1, since nothing there beats it.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINNER_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define MAIN_PRIORITY 0x50
#define T_PRIORITY 0xA0
#define RAISED 0x60
#define SPINS 400000000

enum Mode { SIGNAL_BUSY, SIGNAL_FREE, DELAY, LOWER };

static SceUID sema, spinner;
static volatile int mode, spinner_up, spinner_stop, spinning;
static volatile int first_ran, first_cpu, first_mask, second_ran, second_cpu;

// Waits about us microseconds without blocking, so the main thread keeps its core
static void busy_wait(SceUInt64 us) {
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    while (sceKernelGetProcessTimeWide() - start < us)
        ;
}

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    spinner_up = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop; n++)
        ;
    return 0;
}

static void spinner_end(void) {
    if (!spinning)
        return;
    spinner_stop = 1;
    test_join_thread(spinner);
    spinning = 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    first_cpu = info.currentCpuId;
    first_mask = sceKernelGetThreadCpuAffinityMask(0);
    first_ran = 1;
    if (mode == SIGNAL_BUSY || mode == SIGNAL_FREE) {
        SceUInt32 timeout = 3 * TEST_WAIT_US;
        sceKernelWaitSema(sema, 1, &timeout);
    } else if (mode == DELAY) {
        sceKernelDelayThread(10000);
    } else {
        sceKernelChangeThreadPriority(0, T_PRIORITY);
    }
    sceKernelGetThreadInfo(0, &info);
    second_cpu = info.currentCpuId;
    second_ran = 1;
    return 0;
}

static void run_case(const char *name, int m) {
    mode = m;
    first_ran = second_ran = 0;
    first_cpu = first_mask = second_cpu = -1;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    spinner_up = spinner_stop = 0;
    spinner = sceKernelCreateThread("spinner", spinner_entry, SPINNER_PRIORITY, 0x4000, 0,
                                    SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(spinner, 0, NULL);
    TEST_WAIT_FOR(spinner_up);
    spinning = 1;

    const SceUID t = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    sceKernelChangeThreadPriority(t, RAISED);
    for (int i = 0; i < 200 && !first_ran; i++)
        sceKernelDelayThread(1000);
    const int first_early = first_ran;
    if (!first_early)
        spinner_end();
    TEST_WAIT_FOR(first_ran);

    const int signal = m == SIGNAL_BUSY || m == SIGNAL_FREE;
    SceKernelThreadInfo waiting = { .size = sizeof(waiting) };
    SceKernelThreadInfo after = { .size = sizeof(after) };
    int waiting_mask = -1;
    if (signal) {
        TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema));
        sceKernelGetThreadInfo(t, &waiting);
        waiting_mask = sceKernelGetThreadCpuAffinityMask(t);
        if (m == SIGNAL_FREE)
            spinner_end();
        sceKernelSignalSema(sema, 1);
        busy_wait(10000);
        sceKernelGetThreadInfo(t, &after);
    }
    for (int i = 0; i < 200 && !second_ran; i++)
        sceKernelDelayThread(1000);
    const int second_early = second_ran;
    spinner_end();
    TEST_WAIT_FOR(second_ran);
    // A T left in a queue that no core takes it from gets every core
    const int rescued = !second_ran;
    if (rescued) {
        sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_ALL);
        TEST_WAIT_FOR(second_ran);
    }
    SceUInt timeout = TEST_WAIT_US;
    if (sceKernelWaitThreadEnd(t, NULL, &timeout) >= 0)
        sceKernelDeleteThread(t);
    sceKernelDeleteSema(sema);

    PRINTF(first_early, "%s: T ran within 200 ms of the raise", name);
    PRINTF(first_cpu, "%s: the core T ran on", name);
    PRINTF(first_mask, "%s: the mask T read then", name);
    if (signal) {
        PRINTF(waiting.status, "%s: T's status in its wait", name);
        PRINTF(waiting.currentCpuId, "%s: T's current CPU in its wait", name);
        PRINTF(waiting.lastExecutedCpuId, "%s: T's last CPU in its wait", name);
        PRINTF(waiting.currentCpuAffinityMask, "%s: T's mask in its info in its wait", name);
        PRINTF(waiting_mask, "%s: T's mask from the main thread in its wait", name);
        PRINTF(waiting.currentPriority, "%s: T's priority in its wait", name);
        PRINTF(after.status, "%s: T's status 10 ms after the signal", name);
        PRINTF(after.currentCpuId, "%s: T's current CPU 10 ms after the signal", name);
    }
    PRINTF(second_early, "%s: T went on within 200 ms", name);
    PRINTF(rescued, "%s: T only went on once given every core", name);
    PRINTF(second_cpu, "%s: the core T went on on", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("signaled while core 0 is held", SIGNAL_BUSY);
    run_case("signaled once core 0 is free", SIGNAL_FREE);
    run_case("a 10 ms delay while core 0 is held", DELAY);
    run_case("lowered by itself to 0xA0 while core 0 is held", LOWER);
    test_exit();
    return 0;
}
