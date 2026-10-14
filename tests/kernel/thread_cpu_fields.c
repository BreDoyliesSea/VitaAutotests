// The CPU fields of thread info for a thread pinned to each user core in turn: currentCpuId and
// lastExecutedCpuId as the thread sees them, and as another thread sees them while it spins, while
// it waits and after it ends. Then a spinning thread's affinity changed by another thread, and
// what GetThreadCpuAffinityMask and GetThreadCpuAffinityMask2 report for the calling thread and
// for others.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// How long a spinner spins at most, in loop iterations, so it ends even if the test doesn't stop it
#define SPIN_CAP 400000000

static SceUID sema;
static volatile int ready, stop, self_cpu, self_current, self_last;

// Reports where it runs, spins until told to stop, then waits on the semaphore
static int spinner(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    self_cpu = sceKernelGetCpuId();
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    self_current = info.currentCpuId;
    self_last = info.lastExecutedCpuId;
    ready = 1;
    for (int n = 0; n < SPIN_CAP && !stop; n++)
        ;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    return 0;
}

static SceUID start_spinner(int mask) {
    ready = stop = 0;
    self_cpu = self_current = self_last = 0xDEAD;
    const SceUID thread = sceKernelCreateThread("spinner", spinner, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, mask, NULL);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(ready);
    return thread;
}

// Pins the calling thread to core, and waits until it runs there
static void move_self(int core) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0 << core);
    TEST_WAIT_FOR(sceKernelGetCpuId() == core);
}

static void print_cpu_fields(const char *when, int core, SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    PRINTF(info.status, "core %d, %s: the status", core, when);
    PRINTF(info.currentCpuId, "core %d, %s: the current CPU", core, when);
    PRINTF(info.lastExecutedCpuId, "core %d, %s: the last CPU", core, when);
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);

    for (int core = 0; core < 3; core++) {
        // The main thread runs on another core, so it can watch the spinner
        move_self((core + 1) % 3);
        const SceUID thread = start_spinner(SCE_KERNEL_CPU_MASK_USER_0 << core);
        PRINTF(self_cpu, "core %d: the CPU it reads for itself", core);
        PRINTF(self_current, "core %d: the current CPU in its own info", core);
        PRINTF(self_last, "core %d: the last CPU in its own info", core);
        print_cpu_fields("spinning", core, thread);
        stop = 1;
        TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
        print_cpu_fields("waiting", core, thread);
        sceKernelSignalSema(sema, 1);
        SceUInt timeout = TEST_WAIT_US;
        sceKernelWaitThreadEnd(thread, NULL, &timeout);
        print_cpu_fields("ended", core, thread);
        sceKernelDeleteThread(thread);
    }

    // Moving a spinning thread from core 1 to core 2, from core 0
    move_self(0);
    SceUID thread = start_spinner(SCE_KERNEL_CPU_MASK_USER_1);
    PRINT("move a spinning thread: the change", sceKernelChangeThreadCpuAffinityMask(thread, SCE_KERNEL_CPU_MASK_USER_2));
    PRINT("move a spinning thread: its affinity after it", sceKernelGetThreadCpuAffinityMask(thread));
    SceKernelThreadInfo info = { .size = sizeof(info) };
    for (int i = 0; i < 2000; i++) {
        info.size = sizeof(info);
        sceKernelGetThreadInfo(thread, &info);
        if (info.currentCpuId == 2)
            break;
        sceKernelDelayThread(1000);
    }
    PRINT("move a spinning thread: it runs on core 2 while it spins", info.currentCpuId == 2);
    PRINT("move a spinning thread: the current affinity in its info", info.currentCpuAffinityMask);
    PRINT("move a spinning thread: the initial affinity in its info", info.initCpuAffinityMask);
    stop = 1;
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    info.size = sizeof(info);
    sceKernelGetThreadInfo(thread, &info);
    PRINT("move a spinning thread: the last CPU once it waits", info.lastExecutedCpuId);
    sceKernelSignalSema(sema, 1);
    test_join_thread(thread);

    // Both affinity getters for the calling thread and for others
    PRINT("the main thread's affinity by UID 0", sceKernelGetThreadCpuAffinityMask(0));
    PRINT("the main thread's affinity by its UID", sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()));
    PRINT("GetThreadCpuAffinityMask2 by UID 0", sceKernelGetThreadCpuAffinityMask2(0));
    PRINT("GetThreadCpuAffinityMask2 by its UID", sceKernelGetThreadCpuAffinityMask2(sceKernelGetThreadId()));
    static const int masks[] = { 0, SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_2, 0x70000 };
    for (unsigned int i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        thread = sceKernelCreateThread("mask", spinner, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, masks[i], NULL);
        PRINTF(sceKernelGetThreadCpuAffinityMask(thread), "created with 0x%X: the affinity", masks[i]);
        PRINTF(sceKernelGetThreadCpuAffinityMask2(thread), "created with 0x%X: GetThreadCpuAffinityMask2", masks[i]);
        sceKernelDeleteThread(thread);
    }
    PRINT("GetThreadCpuAffinityMask2 for an unknown UID", sceKernelGetThreadCpuAffinityMask2(0x12345));

    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
