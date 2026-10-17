// Which core a woken thread that may run on every core goes to, when each core runs a thread of
// another priority below its own: the core of the lowest priority, the core of the thread that
// woke it, or the core it last ran on. T waits on a semaphore on its last core, then its mask
// becomes every core. Spinner threads loop without syscalls on cores 0, 1 and 2, at 0xB8 on the
// core of the lowest priority, 0xB4 on T's last core and 0xB0 on the waker's core. The main
// thread, on core 2 above them all, then waits on a semaphore, and once it does the waker's
// spinner signals T, so every core runs its spinner at the signal. T records the core it runs on.
// Each order of the three roles over the cores runs with T at 0x70, in the queues of one core,
// and at 0xA0, in the queue the cores share. A last case gives T cores 0 and 1 alone, last on
// core 0 at 0xB0, core 1 at 0xB4, and the waker on core 2 at 0xB8. core_choice wakes T from
// outside its mask, and wake_core_ties from cores of equal priorities.
//
// On a PS Vita TV on firmware 3.65 T goes to the core that runs the lowest priority in every order
// of the three roles, wherever the waker runs and wherever T last ran, at 0x70 and at 0xA0. With
// the mask of cores 0 and 1 and the waker on core 2 the lowest, T goes to core 1, the lower of its
// two cores.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x48
#define SPINS 400000000

static SceUID sema, done, main_thread, spinners[3];
static volatile int spinner_up[3], spinner_stop[3], waker, waker_go, signaled, t_cpu;

static SceUInt32 mask_of(int core) {
    return SCE_KERNEL_CPU_MASK_USER_0 << core;
}

// Whether the main thread waits on the done semaphore
static int main_waits(void) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(main_thread, &info) == 0 && info.waitType == WAIT_TYPE_SEMA;
}

// Holds its core, and the spinner of the waker's core signals T once the main thread waits
static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    spinner_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop[core]; n++) {
        if (core == waker && waker_go && !signaled && main_waits()) {
            sceKernelSignalSema(sema, 1);
            signaled = 1;
        }
    }
    return 0;
}

static void spinner_start(int core, int priority) {
    spinner_up[core] = spinner_stop[core] = 0;
    spinners[core] = sceKernelCreateThread("spinner", spinner_entry, priority, 0x4000, 0,
                                           mask_of(core), NULL);
    sceKernelStartThread(spinners[core], sizeof(core), &core);
    TEST_WAIT_FOR(spinner_up[core]);
}

static void spinner_end(int core) {
    spinner_stop[core] = 1;
    test_join_thread(spinners[core]);
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    sceKernelSignalSema(done, 1);
    return 0;
}

static void run_case(const char *name, const int priorities[3], int waker_core, int last,
                     SceUInt32 mask, int t_priority) {
    t_cpu = -1;
    waker = waker_core;
    waker_go = signaled = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    done = sceKernelCreateSema("done", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, t_priority, 0x4000, 0, mask_of(last),
                                           NULL);
    sceKernelStartThread(t, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema));
    sceKernelChangeThreadCpuAffinityMask(t, mask);
    for (int core = 0; core < 3; core++)
        spinner_start(core, priorities[core]);
    waker_go = 1;
    SceUInt32 timeout = 4 * TEST_WAIT_US;
    sceKernelWaitSema(done, 1, &timeout);
    for (int core = 0; core < 3; core++)
        spinner_end(core);
    test_join_thread(t);
    sceKernelDeleteSema(sema);
    sceKernelDeleteSema(done);
    PRINTF(t_cpu, "%s, T at 0x%X: the core T ran on", name, t_priority);
}

int main(void) {
    main_thread = sceKernelGetThreadId();
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    // The cores of the lowest priority, the waker and T's last run, in each order
    static const int roles[6][3] = {
        { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 },
    };
    static const int t_priorities[2] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++) {
        for (int r = 0; r < 6; r++) {
            const int lowest = roles[r][0], waker_core = roles[r][1], last = roles[r][2];
            int priorities[3];
            priorities[lowest] = 0xB8;
            priorities[last] = 0xB4;
            priorities[waker_core] = 0xB0;
            char name[96];
            sceClibSnprintf(name, sizeof(name),
                            "the lowest on core %d, the waker on core %d, last on core %d",
                            lowest, waker_core, last);
            run_case(name, priorities, waker_core, last, SCE_KERNEL_CPU_MASK_USER_ALL,
                     t_priorities[p]);
        }
        static const int priorities[3] = { 0xB0, 0xB4, 0xB8 };
        run_case("cores 0 and 1 alone, the waker on core 2 the lowest, last on core 0",
                 priorities, 2, 0, SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1,
                 t_priorities[p]);
    }
    test_exit();
    return 0;
}
