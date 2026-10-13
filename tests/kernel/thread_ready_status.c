// The status sceKernelGetThreadInfo reports for a thread that can run but doesn't, because a
// higher priority thread holds its core: one just started, and one whose wait just ended. Every
// thread is pinned to core 0, and the test threads spin once they run, so what they report can't
// change until the test lets them end.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// Lower than the main thread's 0xA0, so these never run while the main thread does
#define LOW_PRIORITY 0xB0

static SceUID sema;
static volatile int release;

static int spinning_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    while (!release)
        ;
    return 0;
}

static int waiting_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 5 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    while (!release)
        ;
    return 0;
}

static int status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, CPU_MASK_USER_0);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);

    // Started, and ready but not running. Nothing blocks the main thread until the read is done.
    release = 0;
    const SceUID started_thread = sceKernelCreateThread("started", spinning_thread, LOW_PRIORITY, 0x4000, 0, CPU_MASK_USER_0, NULL);
    sceKernelStartThread(started_thread, 0, NULL);
    TEST_WAIT_FOR(status_of(started_thread) != 0x10);
    SceKernelThreadInfo started_info = { .size = sizeof(started_info) };
    sceKernelGetThreadInfo(started_thread, &started_info);
    release = 1;
    test_join_thread(started_thread);

    // A waiter whose wait just ended
    release = 0;
    const SceUID waiter = sceKernelCreateThread("waiter", waiting_thread, LOW_PRIORITY, 0x4000, 0, CPU_MASK_USER_0, NULL);
    sceKernelStartThread(waiter, 0, NULL);
    test_wait_until_waiting(waiter);
    SceKernelThreadInfo waiting_info = { .size = sizeof(waiting_info) };
    sceKernelGetThreadInfo(waiter, &waiting_info);
    sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(status_of(waiter) != 0x8);
    SceKernelThreadInfo woken_info = { .size = sizeof(woken_info) };
    sceKernelGetThreadInfo(waiter, &woken_info);
    release = 1;
    test_join_thread(waiter);

    SceKernelThreadInfo own_info = { .size = sizeof(own_info) };
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &own_info);

    PRINT("started: the status", started_info.status);
    PRINT("started: the current CPU", started_info.currentCpuId);
    PRINT("started: the wait type", started_info.waitType);
    PRINT("waiting: the status", waiting_info.status);
    PRINT("woken: the status", woken_info.status);
    PRINT("woken: the current CPU", woken_info.currentCpuId);
    PRINT("woken: the wait type", woken_info.waitType);
    PRINT("this thread: the status", own_info.status);
    PRINT("this thread: the current CPU", own_info.currentCpuId);

    test_exit();
    return 0;
}
