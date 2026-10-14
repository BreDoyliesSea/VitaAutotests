// What thread info reports for a thread in each state, and what a ForVM suspend changes on top of
// each: dormant, ready behind a higher priority thread on its core, ready and suspended, waiting on
// a semaphore, waiting and suspended, woken while suspended, running on another core, running and
// suspended, delaying, and ended. Also a wait whose timeout passes while the thread is suspended.
// The main thread runs on core 1 and the test thread T on core 0, where a high priority gate thread
// spins without syscalls whenever T must stay ready. For each state the test prints the status,
// the wait type, whether the current CPU is -1, and whether T made progress.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define T_PRIORITY 0xA0
#define SPINS 400000000

static SceUID sema;
static volatile int gate_up, gate_open, ran, woke, stop, wait_result;
static volatile unsigned int counter;

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++)
        ;
    return 0;
}

static SceUID gate;

static void gate_close(void) {
    gate_up = gate_open = 0;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
}

static void gate_lift(void) {
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
}

// Runs, waits on the semaphore, then spins until told to stop
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    ran = 1;
    SceUInt timeout = 3 * TEST_WAIT_US;
    wait_result = sceKernelWaitSema(sema, 1, &timeout);
    woke = 1;
    for (int n = 0; n < SPINS && !stop; n++)
        counter++;
    return 0;
}

static void print_state(const char *name, SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    const int r = sceKernelGetThreadInfo(thread, &info);
    PRINTF(r, "%s: info", name);
    PRINTF(info.status, "%s: the status", name);
    PRINTF(info.waitType, "%s: the wait type", name);
    PRINTF(info.currentCpuId == -1, "%s: the current CPU is -1", name);
}

// Whether counter moves within 50 ms
static int progresses(void) {
    const unsigned int before = counter;
    sceKernelDelayThread(50000);
    return counter != before;
}

static int timed_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 100000;
    wait_result = sceKernelWaitSema(sema, 1, &timeout);
    woke = 1;
    return 0;
}

static int delay_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelDelayThread(500000);
    return 0;
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);

    const SceUID t = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    print_state("dormant", t);

    gate_close();
    sceKernelStartThread(t, 0, NULL);
    print_state("ready behind the gate", t);
    PRINT("ready behind the gate: it ran", ran);
    PRINT("suspend it while ready", sceKernelSuspendThreadForVM(t));
    print_state("ready and suspended", t);
    gate_lift();
    sceKernelDelayThread(50000);
    PRINT("ready and suspended, after the gate: it ran", ran);
    print_state("suspended after the gate", t);
    PRINT("resume it", sceKernelResumeThreadForVM(t));
    TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema));
    PRINT("after the resume: it ran", ran);
    print_state("waiting", t);

    PRINT("suspend it while waiting", sceKernelSuspendThreadForVM(t));
    print_state("waiting and suspended", t);
    PRINT("signal while it is suspended", sceKernelSignalSema(sema, 1));
    sceKernelDelayThread(50000);
    print_state("signaled while suspended", t);
    PRINT("signaled while suspended: it woke", woke);
    SceKernelSemaInfo sema_info = { .size = sizeof(sema_info) };
    sceKernelGetSemaInfo(sema, &sema_info);
    PRINT("signaled while suspended: the count", sema_info.currentCount);
    PRINT("signaled while suspended: the waiters", sema_info.numWaitThreads);
    PRINT("resume it", sceKernelResumeThreadForVM(t));
    TEST_WAIT_FOR(woke);
    PRINT("after the resume: its wait", wait_result);

    PRINT("running: it progresses", progresses());
    print_state("running on core 0", t);
    PRINT("suspend it while running", sceKernelSuspendThreadForVM(t));
    sceKernelDelayThread(10000);
    PRINT("running and suspended: it progresses", progresses());
    print_state("running and suspended", t);
    PRINT("resume it", sceKernelResumeThreadForVM(t));
    PRINT("after the resume: it progresses", progresses());
    stop = 1;
    sceKernelWaitThreadEnd(t, NULL, NULL);
    print_state("ended", t);
    sceKernelDeleteThread(t);

    // A wait whose timeout passes while the thread is suspended
    woke = 0;
    wait_result = 0xDEAD;
    const SceUID timed = sceKernelCreateThread("timed", timed_entry, T_PRIORITY, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(timed, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(timed, WAIT_TYPE_SEMA, sema));
    PRINT("suspend a thread in a wait with a 100 ms timeout", sceKernelSuspendThreadForVM(timed));
    sceKernelDelayThread(300000);
    print_state("its timeout passed while suspended", timed);
    PRINT("its timeout passed while suspended: it woke", woke);
    PRINT("resume it", sceKernelResumeThreadForVM(timed));
    TEST_WAIT_FOR(woke);
    PRINT("after the resume: its wait", wait_result);
    test_join_thread(timed);

    const SceUID delaying = sceKernelCreateThread("delay", delay_entry, T_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(delaying, 0, NULL);
    test_wait_until_waiting(delaying);
    print_state("delaying", delaying);
    test_join_thread(delaying);

    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
