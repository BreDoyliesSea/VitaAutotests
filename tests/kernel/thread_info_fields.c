// The fields sceKernelGetThreadInfo reports over a thread's life: what create sets, what a priority
// or affinity change touches before the start, while it waits and from inside it, and what ending
// and starting again keep
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID go;
static volatile int runs, run_priority, run_affinity, changed_priority, zero_result, zero_priority;

// Reports the priority and affinity it runs with, waits for go, and on its first run raises its
// own priority before it ends
static int fields_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    runs++;
    run_priority = sceKernelGetThreadCurrentPriority();
    run_affinity = sceKernelGetThreadCpuAffinityMask(0);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(go, 1, &timeout);
    if (runs == 1)
        sceKernelChangeThreadPriority(0, 0x50);
    return 0;
}

// Changes its own priority, then to 0
static int zero_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelChangeThreadPriority(0, 0x70);
    changed_priority = sceKernelGetThreadCurrentPriority();
    zero_result = sceKernelChangeThreadPriority(0, 0);
    zero_priority = sceKernelGetThreadCurrentPriority();
    return 0;
}

static void print_fields(const char *name, SceUID thread, SceKernelThreadEntry entry) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    PRINTF(sceKernelGetThreadInfo(thread, &info), "%s: info", name);
    PRINTF(info.processId == sceKernelGetProcessId(), "%s: the process is this one", name);
    if (entry) {
        PRINTF(sceClibStrcmp(info.name, "fields") == 0, "%s: the name is the one given", name);
        PRINTF(info.entry == entry, "%s: the entry is the one given", name);
        PRINTF(info.stackSize, "%s: the stack size", name);
    }
    PRINTF(info.stack != NULL, "%s: the stack has an address", name);
    PRINTF(info.attr, "%s: the attributes", name);
    PRINTF(info.status, "%s: the status", name);
    PRINTF(info.initPriority, "%s: the initial priority", name);
    PRINTF(info.currentPriority, "%s: the current priority", name);
    PRINTF(info.initCpuAffinityMask, "%s: the initial affinity", name);
    PRINTF(info.currentCpuAffinityMask, "%s: the current affinity", name);
    PRINTF(info.currentCpuId, "%s: the current CPU", name);
    PRINTF(info.lastExecutedCpuId, "%s: the last CPU it ran on", name);
    PRINTF(info.waitType, "%s: the wait type", name);
    PRINTF(info.exitStatus, "%s: the exit status", name);
    PRINTF(info.runClocks == 0, "%s: it has run for no time", name);
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    print_fields("main thread", sceKernelGetThreadId(), NULL);

    // Which thread the run status reports on this core
    SceKernelThreadRunStatus run_status = { .size = sizeof(run_status) };
    PRINT("run status", sceKernelGetThreadRunStatus(0, &run_status));
    PRINT("run status: the thread on core 0 is this one", run_status.cpuInfo[0].threadId == sceKernelGetThreadId());
    PRINT("run status: its process is this one", run_status.cpuInfo[0].processId == sceKernelGetProcessId());
    PRINT("run status: its priority", run_status.cpuInfo[0].priority);

    const SceUID thread = sceKernelCreateThread("fields", fields_thread, 0x60, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    print_fields("created", thread, fields_thread);

    // Changes before the first start
    PRINT("change the priority of a thread that never ran", sceKernelChangeThreadPriority(thread, 0x70));
    PRINT("change its affinity", sceKernelChangeThreadCpuAffinityMask(thread, SCE_KERNEL_CPU_MASK_USER_2));
    print_fields("changed before the start", thread, fields_thread);

    // Running, then waiting
    sceKernelStartThread(thread, 0, NULL);
    test_wait_until_waiting(thread);
    PRINT("first run: the priority it runs with", run_priority);
    PRINT("first run: the affinity it runs with", run_affinity);
    print_fields("waiting", thread, fields_thread);
    PRINT("change the priority of the waiting thread", sceKernelChangeThreadPriority(thread, 0x68));
    PRINT("change its affinity", sceKernelChangeThreadCpuAffinityMask(thread, SCE_KERNEL_CPU_MASK_USER_1));
    print_fields("changed while waiting", thread, fields_thread);

    // Ended after raising its own priority
    sceKernelSignalSema(go, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    print_fields("ended", thread, fields_thread);

    // Started again
    sceKernelStartThread(thread, 0, NULL);
    test_wait_until_waiting(thread);
    PRINT("second run: the priority it runs with", run_priority);
    PRINT("second run: the affinity it runs with", run_affinity);
    print_fields("started again", thread, fields_thread);
    sceKernelSignalSema(go, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    // Priority 0 for an ended thread
    PRINT("change the ended thread's priority to 0", sceKernelChangeThreadPriority(thread, 0));
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    PRINT("its priority after the change to 0", info.currentPriority);
    sceKernelDeleteThread(thread);

    // A thread created at 0x60 changing its own priority to 0x70, then to 0
    const SceUID zero = sceKernelCreateThread("zero", zero_thread, 0x60, 0x4000, 0, 0, NULL);
    sceKernelStartThread(zero, 0, NULL);
    sceKernelWaitThreadEnd(zero, NULL, NULL);
    PRINT("own priority: after the change to 0x70", changed_priority);
    PRINT("own priority: change it to 0", zero_result);
    PRINT("own priority: after the change to 0", zero_priority);
    sceKernelDeleteThread(zero);

    test_exit();
    return 0;
}
