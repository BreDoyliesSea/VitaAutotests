// Whether a thread that waits for core 0 moves to core 1 when its mask grows to take in core 1,
// with core 0 still in it. T runs on core 0 alone, behind a gate thread of the highest priority
// that spins there without syscalls, and the main thread runs on core 2. T is made ready in one
// of these ways:
// - STANDBY at 0x70: started from core 2
// - READY at 0x70: woken by a signal from the gate, on core 0 (wake_core_origin)
// - READY at 0xA0: started from core 2, which puts it in the queue the cores share
// Then the main thread changes T's mask to cores 0 and 1, with core 1 idle, or running a spinner
// at 0xB0 without syscalls. Each case prints T's status before the change, whether T ran within
// 200 ms of it, while the gate still held core 0, and the core T ran on. A last case changes the
// mask of a READY T at 0xA0 to core 1 alone, with core 1 idle, which ready_changes does at 0x70.
//
// On a PS Vita TV on firmware 3.65 T at 0x70, STANDBY or READY, stays on core 0 and runs there only
// once the gate ends, with core 1 idle or running the spinner at 0xB0. T at 0xA0, in the queue the
// cores share, runs on core 1 at once, idle or running the spinner, and so it does when its mask
// becomes core 1 alone.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define TWO_CORES (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)
#define SPINS 400000000

enum How { STANDBY, GATE_SIGNAL, SHARED };

static SceUID sema, gate, spinner;
static volatile int gate_up, gate_open, gate_signal, gate_signaled, spinner_up, spinner_stop;
static volatile int t_ran, t_cpu;

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++) {
        if (gate_signal && !gate_signaled) {
            sceKernelSignalSema(sema, 1);
            gate_signaled = 1;
        }
    }
    return 0;
}

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    spinner_up = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop; n++)
        ;
    return 0;
}

// T, which waits for the semaphore first when started with an argument
static int t_entry(SceSize args, void *argp) {
    (void)argp;
    if (args) {
        SceUInt32 timeout = 3 * TEST_WAIT_US;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    t_ran = 1;
    return 0;
}

static void run_case(const char *name, int how, int busy_core_1, SceUInt32 mask) {
    t_ran = 0;
    t_cpu = -2;
    gate_up = gate_open = gate_signal = gate_signaled = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, how == SHARED ? 0xA0 : 0x70, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    if (how == GATE_SIGNAL) {
        int wait = 1;
        sceKernelStartThread(t, sizeof(wait), &wait);
        TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema));
    }
    if (busy_core_1) {
        spinner_up = spinner_stop = 0;
        spinner = sceKernelCreateThread("spinner", spinner_entry, 0xB0, 0x4000, 0,
                                        SCE_KERNEL_CPU_MASK_USER_1, NULL);
        sceKernelStartThread(spinner, 0, NULL);
        TEST_WAIT_FOR(spinner_up);
    }
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0,
                                 SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
    if (how == GATE_SIGNAL) {
        gate_signal = 1;
        TEST_WAIT_FOR(gate_signaled);
    } else {
        sceKernelStartThread(t, 0, NULL);
    }
    sceKernelDelayThread(10000);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(t, &info);

    sceKernelChangeThreadCpuAffinityMask(t, mask);
    for (int i = 0; i < 200 && !t_ran; i++)
        sceKernelDelayThread(1000);
    const int ran_early = t_ran;
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
    TEST_WAIT_FOR(t_ran);
    // Joined first, so every record the thread made is complete
    test_join_thread(t);
    if (busy_core_1) {
        spinner_stop = 1;
        test_join_thread(spinner);
    }
    sceKernelDeleteSema(sema);

    PRINTF(info.status, "%s: T's status before the change", name);
    PRINTF(ran_early, "%s: T ran while the gate held core 0", name);
    PRINTF(t_cpu, "%s: the core T ran on", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("STANDBY at 0x70, core 1 idle", STANDBY, 0, TWO_CORES);
    run_case("READY at 0x70, core 1 idle", GATE_SIGNAL, 0, TWO_CORES);
    run_case("READY at 0xA0, core 1 idle", SHARED, 0, TWO_CORES);
    run_case("STANDBY at 0x70, core 1 busy at 0xB0", STANDBY, 1, TWO_CORES);
    run_case("READY at 0x70, core 1 busy at 0xB0", GATE_SIGNAL, 1, TWO_CORES);
    run_case("READY at 0xA0, core 1 busy at 0xB0", SHARED, 1, TWO_CORES);
    run_case("READY at 0xA0, moved to core 1 alone, core 1 idle", SHARED, 0,
             SCE_KERNEL_CPU_MASK_USER_1);
    test_exit();
    return 0;
}
