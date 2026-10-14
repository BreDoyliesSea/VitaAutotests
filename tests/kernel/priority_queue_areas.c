// Whether a ready thread that may run on cores 0 and 1 moves to whichever of them is free, for a
// thread at priority 0x70 and one at 0xA0. Thread T spins on one of the two cores, then a higher
// priority thread pinned to that core takes it: does T go on running on the other core, or wait
// until its core is free again. Also a T that waits while its last core gets busy and is then
// woken, and a T started while a pinned thread keeps core 0 busy, against one started while core 1
// is busy. Then a T woken while both of its cores are busy, and whether it runs once the core it
// last ran on frees up, or the other one. The main thread runs on core 2, outside T's mask. The output never names a core, only
// whether T ran, and whether it moved.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define BLOCKER_PRIORITY 0x48
#define T_MASK (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)
#define SPINS 400000000

static SceUID sema;
static volatile int stop_t, stop_blocker, blocker_up, t_waits;
static volatile unsigned int counter;

static int t_spin(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (int n = 0; n < SPINS && !stop_t; n++)
        counter++;
    return 0;
}

// Spins, waits on the semaphore, then spins again
static int t_wait(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (int n = 0; n < 1000000; n++)
        counter++;
    t_waits = 1;
    SceUInt timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    for (int n = 0; n < SPINS && !stop_t; n++)
        counter++;
    return 0;
}

static int blocker_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    blocker_up = 1;
    for (volatile int n = 0; n < SPINS && !stop_blocker; n++)
        ;
    return 0;
}

static SceUID blockers[2];

// Keeps the given core busy with a higher priority thread pinned to it
static void block_core_n(int slot, int cpu) {
    stop_blocker = blocker_up = 0;
    blockers[slot] = sceKernelCreateThread("blocker", blocker_entry, BLOCKER_PRIORITY, 0x4000, 0, cpu == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : SCE_KERNEL_CPU_MASK_USER_1, NULL);
    sceKernelStartThread(blockers[slot], 0, NULL);
    TEST_WAIT_FOR(blocker_up);
}

static void unblock_all(void) {
    stop_blocker = 1;
    for (int i = 0; i < 2; i++) {
        if (blockers[i] > 0) {
            sceKernelWaitThreadEnd(blockers[i], NULL, NULL);
            sceKernelDeleteThread(blockers[i]);
            blockers[i] = 0;
        }
    }
}

static void block_core(int cpu) {
    block_core_n(0, cpu);
}

static void unblock_core(void) {
    unblock_all();
}

static volatile int stop_one[2];

static int one_blocker_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    blocker_up = 1;
    for (volatile int n = 0; n < SPINS && !stop_one[i]; n++)
        ;
    return 0;
}

static SceKernelThreadInfo info_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info;
}

// Whether counter moves within 50 ms
static int progresses(void) {
    const unsigned int before = counter;
    sceKernelDelayThread(50000);
    return counter != before;
}

static SceUID start_t(SceKernelThreadEntry entry, int priority) {
    stop_t = t_waits = 0;
    counter = 0;
    const SceUID t = sceKernelCreateThread("t", entry, priority, 0x4000, 0, T_MASK, NULL);
    sceKernelStartThread(t, 0, NULL);
    return t;
}

static void end_t(SceUID t) {
    stop_t = 1;
    sceKernelWaitThreadEnd(t, NULL, NULL);
    sceKernelDeleteThread(t);
}

static void preempted(int priority) {
    const SceUID t = start_t(t_spin, priority);
    TEST_WAIT_FOR(counter != 0);
    const int cpu = info_of(t).currentCpuId;
    block_core(cpu);
    sceKernelDelayThread(10000);
    const int runs = progresses();
    const SceKernelThreadInfo during = info_of(t);
    unblock_core();
    const int runs_after = progresses();
    end_t(t);
    PRINTF(cpu == 0 || cpu == 1, "priority 0x%X: T runs on core 0 or 1", priority);
    PRINTF(runs, "priority 0x%X, a pinned thread takes T's core: T goes on running", priority);
    PRINTF(runs && during.currentCpuId != cpu, "priority 0x%X, a pinned thread takes T's core: T runs on the other core", priority);
    PRINTF(during.status, "priority 0x%X, a pinned thread takes T's core: T's status", priority);
    PRINTF(runs_after, "priority 0x%X, its core is free again: T runs", priority);
}

static void woken(int priority) {
    const SceUID t = start_t(t_wait, priority);
    TEST_WAIT_FOR(t_waits && test_in_wait(t, WAIT_TYPE_SEMA, sema));
    const int cpu = info_of(t).lastExecutedCpuId;
    block_core(cpu);
    sceKernelSignalSema(sema, 1);
    sceKernelDelayThread(10000);
    const int runs = progresses();
    const SceKernelThreadInfo during = info_of(t);
    unblock_core();
    const int runs_after = progresses();
    end_t(t);
    PRINTF(cpu == 0 || cpu == 1, "priority 0x%X: T last ran on core 0 or 1", priority);
    PRINTF(runs, "priority 0x%X, woken while its last core is busy: T runs", priority);
    PRINTF(runs && during.currentCpuId != cpu, "priority 0x%X, woken while its last core is busy: T runs on the other core", priority);
    PRINTF(during.status, "priority 0x%X, woken while its last core is busy: T's status", priority);
    PRINTF(runs_after, "priority 0x%X, its last core is free again: T runs", priority);
}

static void started(int priority, int busy_cpu) {
    block_core(busy_cpu);
    const SceUID t = start_t(t_spin, priority);
    sceKernelDelayThread(10000);
    const int runs = progresses();
    const SceKernelThreadInfo during = info_of(t);
    unblock_core();
    end_t(t);
    PRINTF(runs, "priority 0x%X, started while core %d is busy: T runs", priority, busy_cpu);
    PRINTF(runs && during.currentCpuId == 1 - busy_cpu, "priority 0x%X, started while core %d is busy: T runs on the free core", priority, busy_cpu);
}

// T becomes ready while both of its cores are busy, then the cores free up one at a time: the one
// it last ran on first, or the other one first
static void both_busy(int priority, int last_first) {
    const SceUID t = start_t(t_wait, priority);
    TEST_WAIT_FOR(t_waits && test_in_wait(t, WAIT_TYPE_SEMA, sema));
    const int last = info_of(t).lastExecutedCpuId;
    const int first_cpu = last_first ? last : 1 - last;
    SceUID busy[2];
    for (int i = 0; i < 2; i++) {
        stop_one[i] = 0;
        blocker_up = 0;
        busy[i] = sceKernelCreateThread("blocker", one_blocker_entry, BLOCKER_PRIORITY, 0x4000, 0, i == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : SCE_KERNEL_CPU_MASK_USER_1, NULL);
        sceKernelStartThread(busy[i], sizeof(i), &i);
        TEST_WAIT_FOR(blocker_up);
    }
    sceKernelSignalSema(sema, 1);
    sceKernelDelayThread(10000);
    const int runs_busy = progresses();
    const int status_busy = info_of(t).status;
    stop_one[first_cpu] = 1;
    sceKernelWaitThreadEnd(busy[first_cpu], NULL, NULL);
    sceKernelDelayThread(10000);
    const int runs_first = progresses();
    stop_one[1 - first_cpu] = 1;
    sceKernelWaitThreadEnd(busy[1 - first_cpu], NULL, NULL);
    sceKernelDelayThread(10000);
    const int runs_both = progresses();
    for (int i = 0; i < 2; i++)
        sceKernelDeleteThread(busy[i]);
    end_t(t);
    PRINTF(runs_busy, "priority 0x%X, woken while both its cores are busy: T runs", priority);
    PRINTF(status_busy, "priority 0x%X, woken while both its cores are busy: T's status", priority);
    PRINTF(last == 0 || last == 1, "priority 0x%X: T last ran on core 0 or 1", priority);
    PRINTF(runs_first, "priority 0x%X, then the core it %s frees up: T runs", priority, last_first ? "last ran on" : "didn't last run on");
    PRINTF(runs_both, "priority 0x%X, then the other core frees up too: T runs", priority);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);

    static const int priorities[] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++) {
        preempted(priorities[p]);
        woken(priorities[p]);
        started(priorities[p], 0);
        started(priorities[p], 1);
        both_busy(priorities[p], 0);
        both_busy(priorities[p], 1);
    }

    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
